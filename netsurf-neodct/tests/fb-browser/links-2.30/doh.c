#include "links.h"

struct dnsquery_addr {
	struct status stat;
	unsigned char *url;
	int redirect_cnt;
};

struct dnsquery_doh {
	struct dnsquery *q;
	struct dnsquery_addr ipv4;
	struct dnsquery_addr ipv6;
	int in_progress;
	uttime ttl;
};

#define skip_bytes(n)	do { if (len < (n)) return; len -= (n); start += (n); } while (0)
#define get_byte(b)	do { if (!len--) return; (b) = *start++; } while (0)
#define get_word(w)	do { unsigned char lo, hi; get_byte(hi); get_byte(lo); (w) = (hi << 8) | lo; } while (0)
#define get_dword(d)	do { unsigned short lw, hw; get_word(hw); get_word(lw); (d) = ((unsigned)hw << 16) | lw; } while (0)
#define skip_name()	do while (1) {		\
	unsigned char n;			\
	get_byte(n);				\
	if ((n & 0xc0) == 0) {			\
		if (n == 0)			\
			break;			\
		skip_bytes(n);			\
		continue;			\
	} else if ((n & 0xc0) == 0xc0) {	\
		skip_bytes(1);			\
		break;				\
	} else {				\
		return;				\
	}					\
} while (0)

static void parse_dns_reply(struct dnsquery *q, unsigned char *start, size_t len)
{
	unsigned short status, qdcount, ancount, i;
	struct dnsquery_doh *doh = q->doh;
	int preference = q->addr_preference;
#ifdef SUPPORT_IPV6
	if (preference == ADDR_PREFERENCE_DEFAULT)
		preference = ADDR_PREFERENCE_IPV6;
#endif
	if (!support_ipv6) preference = ADDR_PREFERENCE_IPV4_ONLY;
	skip_bytes(2);
	get_word(status);
	if (status & 0xf)
		return;
	get_word(qdcount);
	get_word(ancount);
	skip_bytes(4);
	for (i = 0; i < qdcount; i++) {
		skip_name();
		skip_bytes(4);
	}
	for (i = 0; i < ancount; i++) {
		unsigned short typ, cls, rdlength;
		unsigned ttl;
		skip_name();
		get_word(typ);
		get_word(cls);
		get_dword(ttl);
		if (ttl < doh->ttl)
			doh->ttl = ttl;
		get_word(rdlength);
		skip_bytes(rdlength);
		/*fprintf(stderr, "typ %04x, class %04x, length %04x\n", typ, cls, rdlength);*/
		if (q->addr) {
			if (cls == 1 && typ == 1 && rdlength == 4) {
				add_address(q->addr, AF_INET, start - rdlength, 0, preference);
			}
#ifdef SUPPORT_IPV6
			if (cls == 1 && typ == 0x1c && rdlength == 16) {
				add_address(q->addr, AF_INET6, start - rdlength, 0, preference);
			}
#endif
		}
	}
}

static void end_doh_lookup(struct dnsquery_doh *doh)
{
	uttime ttl;
	struct dnsquery *q;
	if (--doh->in_progress)
		return;
	q = doh->q;
	ttl = doh->ttl;
	mem_free(doh);
	if (ttl * 1000 / 1000 != ttl)
		ttl = -1;
	else
		ttl *= 1000;
	end_dns_lookup(q, q->addr ? !q->addr->n : 1, ttl);
}

static void doh_end(struct status *stat, void *doh_)
{
	struct dnsquery_doh *doh = (struct dnsquery_doh *)doh_;
	struct dnsquery_addr *da = get_struct(stat, struct dnsquery_addr, stat);
	struct cache_entry *ce;

	if (stat->state >= 0)
		return;

	if (stat->state == S__OK && (ce = stat->ce)) {
		if (ce->redirect && da->redirect_cnt++ < MAX_REDIRECTS) {
			unsigned char *u;
			u = join_urls(da->url, ce->redirect);
			if (!strchr(cast_const_char u, POST_CHAR)) {
				unsigned char *pc = cast_uchar strchr(cast_const_char da->url, POST_CHAR);
				if (pc)
					add_to_strn(&u, pc);
			}
			mem_free(da->url);
			da->url = u;
			load_url(u, NULL, &da->stat, PRI_DOH, NC_RELOAD, 1, 1, 0, 0);
			return;
		}
		if (ce->http_code == 200) {
			unsigned char *start;
			size_t len;
			get_file_by_term(NULL, ce, &start, &len, NULL);
			parse_dns_reply(doh->q, start, len);
		}
	}
	mem_free(da->url);
	end_doh_lookup(doh);
}

static int add_host_name(unsigned char **r, int *l, unsigned char *name)
{
	while (*name) {
		size_t len = strcspn(cast_const_char name, ".");
		if (!len || len > 63)
			return -1;
		add_chr_to_str(r, l, (unsigned char)len);
		add_bytes_to_str(r, l, name, len);
		name += len;
		if (*name == '.')
			name++;
	}
	add_chr_to_str(r, l, 0);
	return 0;
}

void do_doh_lookup(struct dnsquery *q)
{
	int p;
	struct dnsquery_doh *doh;
	int bad_name = 0;

	q->doh = doh = mem_calloc(sizeof(struct dnsquery_doh));
	doh->q = q;
	doh->ttl = -1;

	for (p = 0; p < 2; p++) {
		struct dnsquery_addr *da = !p ? &doh->ipv4 : &doh->ipv6;
		unsigned char *u;
		int ul;
		unsigned char *r;
		int rl;
		int i;

		if (!p) {
#ifdef SUPPORT_IPV6
			if (q->addr_preference == ADDR_PREFERENCE_IPV6_ONLY)
				continue;
#endif
		} else {
#ifndef SUPPORT_IPV6
			continue;
#endif
			if (q->addr_preference == ADDR_PREFERENCE_IPV4_ONLY)
				continue;
		}

		doh->in_progress++;
		da->stat.end = doh_end;
		da->stat.data = doh;

		u = init_str();
		ul = 0;
		memset(q->addr, 0, sizeof(struct lookup_result));
		if (!casecmp(dns_over_https, cast_uchar "http://", 7) || !casecmp(dns_over_https, cast_uchar "https://", 8)) {
			add_to_str(&u, &ul, dns_over_https);
		} else {
			int ipv6 = 0;
			add_to_str(&u, &ul, cast_uchar "https://");
#ifdef SUPPORT_IPV6
			if (!numeric_ipv6_address(dns_over_https, NULL, NULL))
				ipv6 = 1;
#endif
			if (ipv6)
				add_chr_to_str(&u, &ul, '[');
			add_to_str(&u, &ul, dns_over_https);
			if (ipv6)
				add_chr_to_str(&u, &ul, ']');
			add_to_str(&u, &ul, cast_uchar "/dns-query");
		}
		add_chr_to_str(&u, &ul, POST_CHAR);
		add_to_str(&u, &ul, cast_uchar "application/dns-message\n");

		r = init_str();
		rl = 0;
		add_bytes_to_str(&r, &rl, cast_uchar "\0\0\1\0\0\1\0\0\0\0\0\0", 12);
		if (add_host_name(&r, &rl, q->name))
			bad_name = 1;
		if (!p)
			add_bytes_to_str(&r, &rl, cast_uchar "\0\1\0\1", 4);
		else
			add_bytes_to_str(&r, &rl, cast_uchar "\0\34\0\1", 4);

		for (i = 0; i < rl; i++) {
			unsigned char p[3];
			sprintf(cast_char p, "%02x", (int)r[i]);
			add_bytes_to_str(&u, &ul, p, 2);
		}
		mem_free(r);
		da->url = u;
	}
	for (p = 0; p < 2; p++) {
		struct dnsquery_addr *da = !p ? &doh->ipv4 : &doh->ipv6;
		if (da->url) {
			if (bad_name) {
				mem_free(da->url);
				end_doh_lookup(doh);
			} else {
				load_url(da->url, NULL, &da->stat, PRI_DOH, NC_RELOAD, 1, 1, 0, 0);
			}
		}
	}
}
