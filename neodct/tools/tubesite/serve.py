"""Serve one page with one <video> on it, over HTTP, with byte ranges.

Development scaffolding: it is never installed on the phone. NetSurf cannot
play a video, so the point of this server is to be the thing the browser
fails on -- and then, once <video> is intercepted, the thing mpv streams
from. mpv seeks by asking for byte ranges, so ranges are the only part of
this that has to be right.

    neodct/tools/tubesite/serve.py --video ../../knowtheway.avi

Serving from a real socket rather than file:// is deliberate. A file:// URL
would let the whole feature be built without ever proving the phone can pull
video over the network, which is the part most likely to be wrong.
"""

import argparse
import os
import re
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
INDEX = os.path.join(HERE, "index.html")

# The one video, at a fixed URL. The file backing it is chosen at startup,
# so nothing 42 MB long has to live in the repository.
VIDEO_URL = "/watch.avi"

# parse_range's answer when the client asked for bytes past the end. Distinct
# from None ("serve the whole file"), because the two need different statuses.
UNSATISFIABLE = "unsatisfiable"

_RANGE_RE = re.compile(r"^bytes=(\d*)-(\d*)$")

_CONTENT_TYPES = {
    ".avi": "video/x-msvideo",
    ".css": "text/css; charset=utf-8",
    ".html": "text/html; charset=utf-8",
    ".mp4": "video/mp4",
}


def content_type(path):
    return _CONTENT_TYPES.get(os.path.splitext(path)[1].lower(),
                              "application/octet-stream")


def parse_range(header, size):
    """Inclusive (start, end) for a Range header, or None to send it all.

    Anything this does not understand -- a multi-range request, a unit that
    is not bytes, syntax it cannot parse -- returns None rather than an
    error. Serving the whole file is always a correct answer to a range
    request; refusing one is not.
    """
    if not header:
        return None

    match = _RANGE_RE.match(header.strip())
    if match is None:
        return None

    first, last = match.group(1), match.group(2)
    if not first and not last:
        return None

    if not first:
        # "bytes=-N": the final N bytes, or the whole file if N is larger.
        return (max(0, size - int(last)), size - 1)

    start = int(first)
    if start >= size:
        return UNSATISFIABLE
    end = min(int(last), size - 1) if last else size - 1
    if end < start:
        return None
    return (start, end)


class _Handler(BaseHTTPRequestHandler):
    """Whitelist routing: three known URLs, everything else is a 404.

    Nothing here takes a path from the client and turns it into a filename,
    so there is no traversal to defend against.
    """

    protocol_version = "HTTP/1.1"
    video_path = None

    def do_HEAD(self):
        self._respond(body=False)

    def do_GET(self):
        self._respond(body=True)

    def _respond(self, body):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html"):
            self._send_file(INDEX, body)
        elif path == VIDEO_URL:
            self._send_file(self.video_path, body)
        else:
            self.send_error(HTTPStatus.NOT_FOUND)

    def _send_file(self, filename, body):
        try:
            size = os.path.getsize(filename)
            handle = open(filename, "rb")
        except OSError:
            self.send_error(HTTPStatus.NOT_FOUND)
            return

        with handle:
            span = parse_range(self.headers.get("Range"), size)
            if span is UNSATISFIABLE:
                self.send_response(HTTPStatus.REQUESTED_RANGE_NOT_SATISFIABLE)
                self.send_header("Content-Range", "bytes */%d" % size)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return

            if span is None:
                start, end = 0, size - 1
                self.send_response(HTTPStatus.OK)
            else:
                start, end = span
                self.send_response(HTTPStatus.PARTIAL_CONTENT)
                self.send_header("Content-Range",
                                 "bytes %d-%d/%d" % (start, end, size))

            self.send_header("Content-Type", content_type(filename))
            self.send_header("Content-Length", str(end - start + 1))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()

            if body:
                handle.seek(start)
                self._copy(handle, end - start + 1)

    def _copy(self, handle, remaining):
        while remaining > 0:
            chunk = handle.read(min(65536, remaining))
            if not chunk:
                return
            try:
                self.wfile.write(chunk)
            except (BrokenPipeError, ConnectionResetError):
                # mpv closes the socket the moment it seeks. Routine.
                return
            remaining -= len(chunk)

    def log_message(self, fmt, *args):
        print("[tubesite] %s - %s" % (self.address_string(), fmt % args))


def build_server(address, video_path):
    handler = type("_BoundHandler", (_Handler,),
                   {"video_path": os.path.abspath(video_path)})
    return ThreadingHTTPServer(address, handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--video", default=os.path.join(
        HERE, "..", "..", "..", "..", "knowtheway.avi"),
        help="the file served at " + VIDEO_URL)
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--host", default="0.0.0.0",
                        help="0.0.0.0 so the phone and QEMU can both reach it")
    args = parser.parse_args()

    if not os.path.exists(args.video):
        parser.error("no such video: %s" % args.video)

    server = build_server((args.host, args.port), args.video)
    print("[tubesite] http://%s:%d/  serving %s"
          % (args.host, args.port, os.path.abspath(args.video)))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
