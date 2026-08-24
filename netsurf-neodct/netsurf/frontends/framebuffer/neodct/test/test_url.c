/*
 * Tests for neodct_url: URL normalisation and host extraction,
 * matching the behaviour of the old WebKit browser (on_go and
 * host_from_url in tests/neodct-browser/main.py).
 */

#include "test_util.h"
#include "../neodct_url.h"

int main(void)
{
	char buf[256];

	/* bare hostname gets https:// prefix */
	CHECK_STR(neodct_url_normalize("example.com", buf, sizeof(buf)),
		  "https://example.com");

	/* existing scheme is left alone */
	CHECK_STR(neodct_url_normalize("http://example.com", buf, sizeof(buf)),
		  "http://example.com");
	CHECK_STR(neodct_url_normalize("file:///tmp/x.html", buf, sizeof(buf)),
		  "file:///tmp/x.html");

	/* surrounding whitespace is stripped (python .strip()) */
	CHECK_STR(neodct_url_normalize("  example.com  ", buf, sizeof(buf)),
		  "https://example.com");

	/* empty or whitespace-only input yields NULL (old code: no-op) */
	CHECK(neodct_url_normalize("", buf, sizeof(buf)) == NULL);
	CHECK(neodct_url_normalize("   ", buf, sizeof(buf)) == NULL);
	CHECK(neodct_url_normalize(NULL, buf, sizeof(buf)) == NULL);

	/* result too long for the buffer yields NULL, not truncation */
	CHECK(neodct_url_normalize("example.com", buf, 12) == NULL);

	/* host_from_url: scheme, path and query stripped */
	CHECK_STR(neodct_host_from_url("https://www.foo.org/bar/baz?q=1",
				       buf, sizeof(buf)),
		  "www.foo.org");
	CHECK_STR(neodct_host_from_url("www.foo.org/bar", buf, sizeof(buf)),
		  "www.foo.org");
	CHECK_STR(neodct_host_from_url("foo.org", buf, sizeof(buf)),
		  "foo.org");
	/* query without path */
	CHECK_STR(neodct_host_from_url("foo.org?q=1", buf, sizeof(buf)),
		  "foo.org");
	/* degenerate input: falls back to input (old code returned url) */
	CHECK_STR(neodct_host_from_url("://", buf, sizeof(buf)), "://");

	TEST_EXIT();
}
