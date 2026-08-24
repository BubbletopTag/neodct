"""neodct/tools/tubesite: the local page the browser work is developed against.

Not shipped to the phone. It exists so <video> interception can be built
against a real server rather than a file:// URL -- mpv has to stream the
thing over HTTP, and that means byte ranges have to work.
"""

import os
import sys
import urllib.error
import urllib.request

import pytest

TOOLS_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "tools"
)
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

from tubesite import serve


# --- range parsing --------------------------------------------------------

def test_no_range_header_serves_whole_file():
    assert serve.parse_range(None, 1000) is None


def test_closed_range():
    assert serve.parse_range("bytes=0-99", 1000) == (0, 99)


def test_open_ended_range_runs_to_end_of_file():
    assert serve.parse_range("bytes=100-", 1000) == (100, 999)


def test_suffix_range_counts_back_from_the_end():
    assert serve.parse_range("bytes=-100", 1000) == (900, 999)


def test_suffix_longer_than_the_file_clamps_to_the_whole_file():
    assert serve.parse_range("bytes=-5000", 1000) == (0, 999)


def test_end_past_the_file_clamps():
    assert serve.parse_range("bytes=900-5000", 1000) == (900, 999)


def test_start_past_the_file_is_unsatisfiable():
    assert serve.parse_range("bytes=1000-", 1000) is serve.UNSATISFIABLE


def test_malformed_range_serves_whole_file():
    assert serve.parse_range("bytes=abc", 1000) is None
    assert serve.parse_range("furlongs=0-9", 1000) is None
    assert serve.parse_range("bytes=", 1000) is None


def test_multi_range_is_not_supported_and_serves_whole_file():
    # A real server would answer multipart/byteranges; mpv never asks.
    assert serve.parse_range("bytes=0-9,20-29", 1000) is None


# --- content types --------------------------------------------------------

def test_avi_gets_a_video_content_type():
    assert serve.content_type("watch.avi") == "video/x-msvideo"


def test_html_and_css_are_utf8():
    assert serve.content_type("index.html") == "text/html; charset=utf-8"


def test_unknown_extension_falls_back_to_octet_stream():
    assert serve.content_type("thing.zzz") == "application/octet-stream"


# --- the server itself ----------------------------------------------------

@pytest.fixture
def site(tmp_path):
    """A running server with a small stand-in for the real video."""
    video = tmp_path / "clip.avi"
    video.write_bytes(bytes(range(256)) * 4)      # 1024 bytes, checkable
    server = serve.build_server(("127.0.0.1", 0), str(video))
    import threading
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield "http://127.0.0.1:%d" % server.server_address[1]
    finally:
        server.shutdown()
        server.server_close()


def test_index_is_served_at_the_root(site):
    with urllib.request.urlopen(site + "/") as response:
        assert response.status == 200
        body = response.read().decode()
    assert "<video" in body


def test_the_page_points_at_the_video_url(site):
    with urllib.request.urlopen(site + "/") as response:
        body = response.read().decode()
    assert serve.VIDEO_URL in body


def test_the_page_carries_no_javascript(site):
    with urllib.request.urlopen(site + "/") as response:
        body = response.read().decode().lower()
    assert "<script" not in body
    assert "onclick" not in body


def test_video_is_served_whole_with_a_length(site):
    with urllib.request.urlopen(site + serve.VIDEO_URL) as response:
        assert response.status == 200
        assert response.headers["Content-Type"] == "video/x-msvideo"
        assert response.headers["Accept-Ranges"] == "bytes"
        assert int(response.headers["Content-Length"]) == 1024
        assert len(response.read()) == 1024


def test_a_range_request_gets_exactly_those_bytes(site):
    request = urllib.request.Request(site + serve.VIDEO_URL)
    request.add_header("Range", "bytes=10-19")
    with urllib.request.urlopen(request) as response:
        assert response.status == 206
        assert response.headers["Content-Range"] == "bytes 10-19/1024"
        assert response.read() == bytes(range(10, 20))


def test_an_unsatisfiable_range_is_refused(site):
    request = urllib.request.Request(site + serve.VIDEO_URL)
    request.add_header("Range", "bytes=99999-")
    with pytest.raises(urllib.error.HTTPError) as excinfo:
        urllib.request.urlopen(request)
    assert excinfo.value.code == 416


def test_paths_outside_the_site_are_refused(site):
    with pytest.raises(urllib.error.HTTPError) as excinfo:
        urllib.request.urlopen(site + "/../../../etc/passwd")
    assert excinfo.value.code in (403, 404)
