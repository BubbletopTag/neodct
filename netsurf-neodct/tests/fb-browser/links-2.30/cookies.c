/* cookies.c
 * Cookies
 * (c) 2002 Mikulas Patocka
 * This file is a part of the Links program, released under GPL
 */

#include "links.h"

#define ACCEPT_NONE	0
#define ACCEPT_ALL	1

struct list_head all_cookies = { &all_cookies, &all_cookies };

struct list_head c_domains = { &c_domains, &c_domains };

void free_cookie(struct cookie *c)
{
	mem_free(c->name);
	if (c->value) mem_free(c->value);
	mem_free(c->server);
	mem_free(c->path);
	mem_free(c->domain);
	mem_free(c);
}

static void accept_cookie(struct cookie *);

/* sezere 1 cookie z retezce str, na zacatku nesmi byt zadne whitechars
 * na konci muze byt strednik nebo 0
 * cookie musi byt ve tvaru nazev=hodnota, kolem rovnase nesmi byt zadne mezery
 * (respektive mezery se budou pocitat do nazvu a do hodnoty)
 */
void set_cookie(unsigned char *url, unsigned char *str)
{
	int noval = 0;
	struct cookie *cookie;
	unsigned char *p, *q, *s, *server, *date, *max_age, *dom;
	if (!enable_cookies)
		return;
	for (p = str; *p != ';' && *p; p++) { /*if (WHITECHAR(*p)) return;*/ }
	for (q = str; *q != '='; q++) if (!*q || q >= p) {
		noval = 1;
		break;
	}
	if (str == q || q + 1 == p) return;
	cookie = mem_alloc(sizeof(struct cookie));
	server = get_host_name(url);
	cookie->saved_cookie = 0;
	cookie->name = memacpy(str, q - str);
	cookie->value = !noval ? memacpy(q + 1, p - q - 1) : NULL;
	cookie->server = stracpy(server);
	cookie->created = get_absolute_seconds();
	cookie->expires = (time_t)-1;
	date = parse_header_param(str, cast_uchar "expires", 0);
	if (date) {
		cookie->expires = parse_http_date(date);
		/* if (!cookie->expires) debug("unable to parse '%s'", date); */
		mem_free(date);
	}
	max_age = parse_header_param(str, cast_uchar "max-age", 0);
	if (max_age) {
		char *end;
		long ma = strtol(cast_const_char max_age, &end, 10);
		if (*max_age && !*end) {
			if (ma < 0) {
				cookie->expires = 0;
			} else {
				cookie->expires = (time_t)((uttime)cookie->created + (uttime)ma);
			}
		}
		mem_free(max_age);
	}
	if (!(cookie->path = parse_header_param(str, cast_uchar "path", 0))) {
		cookie->path = stracpy(cast_uchar "/");
	} else {
		if (cookie->path[0] != '/') {
			add_to_strn(&cookie->path, cast_uchar "x");
			memmove(cookie->path + 1, cookie->path, strlen(cast_const_char cookie->path) - 1);
			cookie->path[0] = '/';
		}
	}
	dom = parse_header_param(str, cast_uchar "domain", 0);
	if (!dom) {
		cookie->domain = stracpy(server);
	} else {
		cookie->domain = idn_encode_host(dom, (int)strlen(cast_const_char dom), cast_uchar ".", 0);
		if (!cookie->domain)
			cookie->domain = stracpy(server);
		mem_free(dom);
	}
	if (cookie->domain[0] == '.') memmove(cookie->domain, cookie->domain + 1, strlen(cast_const_char cookie->domain));
	if ((s = parse_header_param(str, cast_uchar "secure", 0))) {
		cookie->secure = 1;
		mem_free(s);
	} else cookie->secure = 0;
	if (!allow_cookie_domain(server, cookie->domain)) {
		mem_free(cookie->domain);
		cookie->domain = stracpy(server);
	}
	accept_cookie(cookie);
	mem_free(server);
}

static void accept_cookie(struct cookie *c)
{
	struct c_domain *cd;
	struct list_head *lcd;
	struct cookie *d;
	struct list_head *ld;
	size_t sl;
	foreach(struct cookie, d, ld, all_cookies) if (!casestrcmp(d->name, c->name) && !casestrcmp(d->domain, c->domain)) {
		ld = ld->prev;
		del_from_list(d);
		free_cookie(d);
	}
	if (c->value && !casestrcmp(c->value, cast_uchar "deleted")) {
		free_cookie(c);
		return;
	}
	add_to_list(all_cookies, c);
	foreach(struct c_domain, cd, lcd, c_domains) if (!casestrcmp(cd->domain, c->domain)) return;
	sl = strlen(cast_const_char c->domain);
	if (sl > MAXINT - sizeof(struct c_domain)) overalloc();
	cd = mem_alloc(sizeof(struct c_domain) + sl);
	strcpy(cast_char cd->domain, cast_const_char c->domain);
	add_to_list(c_domains, cd);
}

int is_in_domain(unsigned char *d, unsigned char *s)
{
	int dl = (int)strlen(cast_const_char d);
	int sl = (int)strlen(cast_const_char s);
	if (dl > sl) return 0;
	if (dl == sl) return !casestrcmp(d, s);
	if (s[sl - dl - 1] != '.') return 0;
	return !casecmp(d, s + sl - dl, dl);
}

int is_path_prefix(unsigned char *d, unsigned char *s)
{
	int dl = (int)strlen(cast_const_char d);
	int sl = (int)strlen(cast_const_char s);
	if (!dl) return 1;
	if (dl > sl) return 0;
	if (memcmp(d, s, dl)) return 0;
	return d[dl - 1] == '/' || !s[dl] || s[dl] == '/' || s[dl] == POST_CHAR || s[dl] == '?' || s[dl] == '&';
}

int cookie_expired(struct cookie *c, time_t now)
{
	if (c->expires != (time_t)-1 && c->expires < now)
		return 1;
	if (max_cookie_age && now - c->created > max_cookie_age * 86400)
		return 1;
	return 0;
}

void add_cookies(unsigned char **s, int *l, unsigned char *url)
{
	int nc = 0;
	struct c_domain *cd;
	struct list_head *lcd;
	struct cookie *c;
	struct list_head *lc;
	unsigned char *server, *data;
	time_t now;
	if (!enable_cookies)
		return;
	server = get_host_name(url);
	data = get_url_data(url);
	if (data > url) data--;
	foreach(struct c_domain, cd, lcd, c_domains) if (is_in_domain(cd->domain, server)) goto ok;
	mem_free(server);
	return;
ok:
	now = get_absolute_seconds();
	foreachback(struct cookie, c, lc, all_cookies) if (is_in_domain(c->domain, server)) if (is_path_prefix(c->path, data)) {
		if (cookie_expired(c, now)) {
			lc = lc->prev;
			del_from_list(c);
			free_cookie(c);
			continue;
		}
		if (c->saved_cookie && !save_cookies) continue;
		if (c->secure && casecmp(url, cast_uchar "https://", 8)) continue;
		if (!nc) add_to_str(s, l, cast_uchar "Cookie: "), nc = 1;
		else add_to_str(s, l, cast_uchar "; ");
		add_to_str(s, l, c->name);
		if (c->value) {
			add_chr_to_str(s, l, '=');
			add_to_str(s, l, c->value);
		}
	}
	if (nc) add_to_str(s, l, cast_uchar "\r\n");
	mem_free(server);
}

void clear_cookies_file(void)
{
	unsigned char *cookies_file;

	if (!links_home)
		return;

	cookies_file = stracpy(links_home);
	add_to_strn(&cookies_file, cast_uchar "cookies.txt");

	delete_config_file(cookies_file);

	mem_free(cookies_file);
}

void do_save_cookies(void)
{
	unsigned char *cookies_file;
	unsigned char *s;
	int sl;
	time_t now;
	struct cookie *c;
	struct list_head *lc;

	if (anonymous || !save_cookies || proxies.only_proxies)
		return;

	if (!links_home)
		return;

	s = init_str();
	sl = 0;

	cookies_file = stracpy(links_home);
	add_to_strn(&cookies_file, cast_uchar "cookies.txt");

	now = get_absolute_seconds();
	foreachback(struct cookie, c, lc, all_cookies) {
		if (cookie_expired(c, now) || c->expires == (time_t)-1)
			continue;
		add_quoted_to_str(&s, &sl, c->domain);
		add_chr_to_str(&s, &sl, ' ');
		add_quoted_to_str(&s, &sl, c->server);
		add_chr_to_str(&s, &sl, ' ');
		add_quoted_to_str(&s, &sl, c->path);
		add_chr_to_str(&s, &sl, ' ');
		add_quoted_to_str(&s, &sl, c->name);
		add_chr_to_str(&s, &sl, ' ');
		add_quoted_to_str(&s, &sl, c->value ? c->value : cast_uchar "");
		add_chr_to_str(&s, &sl, ' ');
		add_chr_to_str(&s, &sl, '0' + c->secure);
		add_chr_to_str(&s, &sl, ' ');
		add_chr_to_str(&s, &sl, '"');
		add_to_str(&s, &sl, print_http_date(c->created));
		add_chr_to_str(&s, &sl, '"');
		add_chr_to_str(&s, &sl, ' ');
		add_chr_to_str(&s, &sl, '"');
		add_to_str(&s, &sl, print_http_date(c->expires));
		add_chr_to_str(&s, &sl, '"');
		add_to_str(&s, &sl, cast_uchar NEWLINE);
	}

	write_to_config_file(cookies_file, s, 0);

	mem_free(cookies_file);
	mem_free(s);
}

unsigned long free_cookies(void)
{
	unsigned long cnt = 0;
	time_t now = get_absolute_seconds();
	free_list(struct c_domain, c_domains);
	while (!list_empty(all_cookies)) {
		struct cookie *c = list_struct(all_cookies.next, struct cookie);
		if (!cookie_expired(c, now))
			cnt++;
		del_from_list(c);
		free_cookie(c);
	}
	return cnt;
}

static time_t parse_cookie_time(unsigned char *t)
{
	time_t tm;
	if (t[strspn(cast_const_char t, "0123456789")] != 0) {
		tm = parse_http_date(t);
	} else {
		char *end;
		tm = (time_t)strtod(cast_const_char t, &end);
		if (*end)
			tm = (time_t)-1;
	}
	if (tm == (time_t)-1) {
		fprintf(stderr, "Invalid cookie time '%s'\n", t);
		return (time_t)-1;
	}
	return tm;
}

void init_cookies(void)
{
	int err = 0;
	unsigned char *cookies_file;
	unsigned char *s, *p;

	if (anonymous || proxies.only_proxies)
		return;

	if (!links_home)
		return;

	cookies_file = stracpy(links_home);
	add_to_strn(&cookies_file, cast_uchar "cookies.txt");

	s = read_config_file(cookies_file);

	p = s;
	if (s) while (*p) {
		unsigned char *l, *line;
		unsigned char *domain, *server, *path, *name, *value,  *secure, *created, *expires; 

		if (*p == '\r' || *p == '\n') {
			p++;
			continue;
		}

		l = p;
		while (*l && *l != '\r' && *l != '\n')
			l++;
		line = memacpy(p, l - p);
		p = l;
		l = line;

		domain = get_token(&l);
		server = get_token(&l);
		path = get_token(&l);
		name = get_token(&l);
		value = get_token(&l);
		secure = get_token(&l);
		created = get_token(&l);
		expires = get_token(&l);
		mem_free(line);

		/*debug("%s %s %s %s %s %s %s %s", name, value, server, path, domain, secure, created expires);*/

		if (expires) {
			struct cookie *c;
			time_t created_time, expire_time;

			created_time = parse_cookie_time(created);
			expire_time = parse_cookie_time(expires);

			if (created_time == (time_t)-1 || expire_time == (time_t)-1)
				goto er;

			c = mem_alloc(sizeof(struct cookie));
			c->saved_cookie = 1;
			c->name = name, name = NULL;
			if (*value)
				c->value = value, value = NULL;
			else
				c->value = NULL;
			c->server = server, server = NULL;
			c->path = path, path = NULL;
			c->domain = domain, domain = NULL;
			c->secure = *secure >= '1';
			c->created = created_time;
			c->expires = expire_time;
			accept_cookie(c);
		}

er:
		if (domain) mem_free(domain);
		if (server) mem_free(server);
		if (path) mem_free(path);
		if (name) mem_free(name);
		if (value) mem_free(value);
		if (secure) mem_free(secure);
		if (created) mem_free(created);
		if (expires) mem_free(expires);
	}

	mem_free(cookies_file);
	if (s)
		mem_free(s);

	if (err) fprintf(stderr, "\007"), portable_sleep(1000);

	/*{
		time_t nt, t;
		nt = get_absolute_seconds();
		unsigned char *now;
		now = print_http_date(nt);
		debug("now: %s", now);
		t = parse_http_date(now);
		debug("%ld - %ld", t, nt);
		now = print_http_date(t);
		debug("now: %s", now);
	}*/
}
