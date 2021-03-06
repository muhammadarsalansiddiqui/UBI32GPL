/*
 *  Match a string against a list of patterns/regexes.
 *
 *  Copyright (C) 2007-2008 Sourcefire, Inc.
 *
 *  Authors: Török Edvin
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#ifndef CL_DEBUG
#define NDEBUG
#endif

#ifdef CL_THREAD_SAFE
#ifndef _REENTRANT
#define _REENTRANT
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <zlib.h>

#include <limits.h>
#include <sys/types.h>
#include <assert.h>


#include "regex/regex.h"


#include "clamav.h"
#include "others.h"
#include "regex_list.h"
#include "matcher-ac.h"
#include "matcher.h"
#include "str.h"
#include "readdb.h"
#include "jsparse/textbuf.h"
#include "regex_suffix.h"
/* Prototypes */
static regex_t *new_preg(struct regex_matcher *matcher);
static size_t reverse_string(char *pattern);
static int add_pattern_suffix(void *cbdata, const char *suffix, size_t suffix_len, const struct regex_list *regex);
static int add_static_pattern(struct regex_matcher *matcher, char* pattern);
/* ---------- */

/* ----- shift-or filtering -------------- */

#define BITMAP_CONTAINS(bmap, val) ((bmap)[(val) >> 5] & (1 << ((val) & 0x1f)))
#define BITMAP_INSERT(bmap, val) ((bmap)[(val) >> 5] |= (1 << ((val) & 0x1f)))

static void SO_init(struct filter *m)
{
	memset(m->B, ~0, sizeof(m->B));
	memset(m->end, ~0, sizeof(m->end));
	memset(m->end_fast, ~0, sizeof(m->end_fast));
}

/* because we use uint32_t */
#define MAXSOPATLEN 32

/* merge another pattern into the filter
 * add('abc'); add('bcd'); will match [ab][bc][cd] */
static int SO_preprocess_add(struct filter *m, const unsigned char *pattern, size_t len)
{
	uint16_t q;
	uint8_t j;

	/* cut length, and make it modulo 2 */
	if(len > MAXSOPATLEN) {
		len = MAXSOPATLEN;
	} else {
		/* we use 2-grams, must be multiple of 2 */
		len = len & ~1;
	}
	if(!len)
		return 0;

	/* Shift-Or like preprocessing */
	for(j=0;j < len-1;j++) {
		/* use overlapping 2-grams. We need them overlapping because matching can start at any position */
		q = cli_readint16( &pattern[j] );
		m->B[q] &= ~(1 << j);
	}
	/* we use variable length patterns, use last character to mark pattern end,
	 * can lead to false positives.*/
	/* mark that at state j, the q-gram q can end the pattern */
	if(j) {
		j--;
		m->end[q] &= ~(1 << j);
		m->end_fast[pattern[j+1]] &= ~(1<<j);
	}
	return 0;
}

/* this is like a FSM, with multiple active states at the same time.
 * each bit in "state" means an active state, when a char is encountered
 * we determine what states can remain active.
 * The FSM transition rules are expressed as bit-masks */
long SO_search(const struct filter *m, const unsigned char *data, unsigned long len)
{
	size_t j;
	uint32_t state = ~0;
	const uint32_t *B = m->B;
	const uint32_t *End = m->end;
	const uint32_t *EndFast = m->end_fast;

	/* cut length, and make it modulo 2 */
	if(len > MAXSOPATLEN) {
		len = MAXSOPATLEN;
	} else {
		/* we use 2-grams, must be multiple of 2 */
		len = len & ~1;
	}
	if(!len) return -1;
	/* Shift-Or like search algorithm */
	for(j=0;j < len-1; j++) {
		const uint16_t q0 = cli_readint16( &data[j] );
		uint32_t match_end;
		state = (state << 1) | B[q0];
		/* state marks with a 0 bit all active states
		 * End[q0] marks with a 0 bit all states where the q-gram 'q' can end a pattern
		 * if we got two 0's at matching positions, it means we encountered a pattern's end */
		match_end = state | EndFast[data[j+1]];
		if((match_end != 0xffffffff) && (state | End[q0]) !=  0xffffffff) {
			/* note: we rely on short-circuit eval here, we only evaluate and fetch End[q0], if
			 * end_fast has matched. This reduces cache pressure on End[], and allows us to keep the working
			 * set inside L2 */

			/* if state is reachable, and this character can finish a pattern, assume match */
			/* to reduce false positives check if qgram can finish the pattern */
			/* return position of probable match */
			/* find first 0 starting from MSB, the position of that bit as counted from LSB, is the length of the
			 * longest pattern that could match */
			return j >= MAXSOPATLEN  ? j - MAXSOPATLEN : 0;
		}
	}
	/* no match */
	return -1;
}

/* ----------------------------------------------------------- */


#define MATCH_SUCCESS 0 
#define MATCH_FAILED  -1

/*
 * Call this function when an unrecoverable error has occured, (instead of exit).
 */
static void fatal_error(struct regex_matcher* matcher)
{
	regex_list_done(matcher);
	matcher->list_inited = -1;/* the phishing module will know we tried to load a whitelist, and failed, so it will disable itself too*/
}


static inline size_t get_char_at_pos_with_skip(const struct pre_fixup_info* info, const char* buffer, size_t pos)
{
	const char* str;
	size_t realpos = 0;
	if(!info) {
		return (pos <= strlen(buffer)) ? buffer[pos>0 ? pos-1:0] : '\0';
	}
	str = info->pre_displayLink.data;
	cli_dbgmsg("calc_pos_with_skip: skip:%lu, %lu - %lu \"%s\",\"%s\"\n", pos, info->host_start, info->host_end, str, buffer);
	pos += info->host_start;
	while(str[realpos] && !isalnum(str[realpos])) realpos++;
	for(; str[realpos] && (pos>0); pos--) {
		while(str[realpos]==' ') realpos++;
		realpos++;
	}
	while(str[realpos]==' ') realpos++;
	cli_dbgmsg("calc_pos_with_skip:%s\n",str+realpos);
	return (pos>0 && !str[realpos]) ? '\0' : str[realpos>0?realpos-1:0];
}

static int validate_subdomain(const struct regex_list *regex, const struct pre_fixup_info *pre_fixup, const char *buffer, size_t buffer_len, char *real_url, size_t real_len, char *orig_real_url)
{
	char c;
	size_t match_len;

	if(!regex || !regex->pattern)
		return 0;
	match_len = strlen(regex->pattern);
	if(((c=get_char_at_pos_with_skip(pre_fixup,buffer,buffer_len+1))==' ' || c=='\0' || c=='/' || c=='?') &&
			(match_len == buffer_len || /* full match */
			 (match_len < buffer_len &&
			  ((c=get_char_at_pos_with_skip(pre_fixup,buffer,buffer_len-match_len))=='.' || (c==' ')) )
			 /* subdomain matched*/)) {
		/* we have an extra / at the end */
		if(match_len > 0) match_len--;
		cli_dbgmsg("Got a match: %s with %s\n", buffer, regex->pattern);
		cli_dbgmsg("Before inserting .: %s\n", orig_real_url);
		if(real_len >= match_len + 1) {
			const size_t pos = real_len - match_len - 1;
			if(real_url[pos] != '.') {
				/* we need to shift left, and insert a '.'
				 * we have an extra '.' at the beginning inserted by get_host to have room,
				 * orig_real_url has to be used here, 
				 * because we want to overwrite that extra '.' */
				size_t orig_real_len = strlen(orig_real_url);
				cli_dbgmsg("No dot here:%s\n",real_url+pos);
				real_url = orig_real_url;
				memmove(real_url, real_url+1, orig_real_len-match_len-1);
				real_url[orig_real_len-match_len-1]='.';
				cli_dbgmsg("After inserting .: %s\n", real_url);
			}
		}
		return 1;
	}
	cli_dbgmsg("Ignoring false match: %s with %s, mismatched character: %c\n", buffer, regex->pattern, c);
	return 0;
}

/*
 * @matcher - matcher structure to use
 * @real_url - href target
 * @display_url - <a> tag contents
 * @hostOnly - if you want to match only the host part
 * @is_whitelist - is this a lookup in whitelist?
 *
 * @return - CL_SUCCESS - url doesn't match
 *         - CL_VIRUS - url matches list
 *
 * Do not send NULL pointers to this function!!
 *
 */
int regex_list_match(struct regex_matcher* matcher,char* real_url,const char* display_url,const struct pre_fixup_info* pre_fixup,int hostOnly,const char **info, int is_whitelist)
{
	char* orig_real_url = real_url;
	struct regex_list *regex;
	size_t real_len, display_len, buffer_len;

	assert(matcher);
	assert(real_url);
	assert(display_url);
	*info = NULL;
	if(!matcher->list_inited)
		return 0;
	assert(matcher->list_built);
	/* skip initial '.' inserted by get_host */
	if(real_url[0] == '.') real_url++;
	if(display_url[0] == '.') display_url++;
	real_len    = strlen(real_url);
	display_len = strlen(display_url);
	buffer_len  = (hostOnly && !is_whitelist) ? real_len + 1 : real_len + display_len + 1 + 1;
	if(buffer_len < 3) {
		/* too short, no match possible */
		return 0;
	}
	{
		char *buffer = cli_malloc(buffer_len+1);
		char *bufrev;
		int rc = 0;
		struct cli_ac_data mdata;
		struct cli_ac_result *res = NULL;

		if(!buffer)
			return CL_EMEM;

		strncpy(buffer,real_url,real_len);
		buffer[real_len]= (!is_whitelist && hostOnly) ? '/' : ':';
		if(!hostOnly || is_whitelist) {
			strncpy(buffer+real_len+1,display_url,display_len);
		}
		buffer[buffer_len - 1] = '/';
		buffer[buffer_len]=0;
		cli_dbgmsg("Looking up in regex_list: %s\n", buffer);

		if((rc = cli_ac_initdata(&mdata, 0, 0, AC_DEFAULT_TRACKLEN)))
			return rc;

		bufrev = cli_strdup(buffer);
		if(!bufrev)
			return CL_EMEM;
		reverse_string(bufrev);
		rc = SO_search(&matcher->filter, (const unsigned char*)bufrev, buffer_len) != -1;
		if(rc == -1) {
			free(buffer);
			free(bufrev);
			/* filter says this suffix doesn't match.
			 * The filter has false positives, but no false
			 * negatives */
			return 0;
		}
		rc = cli_ac_scanbuff((const unsigned char*)bufrev,buffer_len, NULL, (void*)&regex, &res, &matcher->suffixes,&mdata,0,0,-1,NULL,AC_SCAN_VIR,NULL);
		free(bufrev);
		cli_ac_freedata(&mdata);

		rc = 0;
		while(res) {
			struct cli_ac_result *q;
			regex = res->customdata;
			while(!rc && regex) {
				/* loop over multiple regexes corresponding to
				 * this suffix */
				if (!regex->preg) {
					/* we matched a static pattern */
					rc = validate_subdomain(regex, pre_fixup, buffer, buffer_len, real_url, real_len, orig_real_url);
				} else {
					rc = !cli_regexec(regex->preg, buffer, 0, NULL, 0);
				}
				if(rc) *info = regex->pattern;
				regex = regex->nxt;
			}
			q = res;
			res = res->next;
			free(q);
		}
		free(buffer);
		if(!rc)
			cli_dbgmsg("Lookup result: not in regex list\n");
		else
			cli_dbgmsg("Lookup result: in regex list\n");
		return rc;
	}
}


/* Initialization & loading */
/* Initializes @matcher, allocating necesarry substructures */
int init_regex_list(struct regex_matcher* matcher)
{
	int rc;

	assert(matcher);
	memset(matcher, 0, sizeof(*matcher));

	matcher->list_inited=1;
	matcher->list_built=0;
	matcher->list_loaded=0;

	hashtab_init(&matcher->suffix_hash, 10);
	if((rc = cli_ac_init(&matcher->suffixes, 2, 32))) {
		return rc;
	}
	if((rc = cli_bm_init(&matcher->md5_hashes))) {
		return rc;
	}
	SO_init(&matcher->filter);
	SO_init(&matcher->md5_filter);
	return CL_SUCCESS;
}

static int functionality_level_check(char* line)
{
	char* ptmin;
	char* ptmax;
	size_t j;

	ptmin = strrchr(line,':');
	if(!ptmin) 
		return CL_SUCCESS;
	
	ptmin++;

	ptmax = strchr(ptmin,'-');
	if(!ptmax) 
		return CL_SUCCESS;/* there is no functionality level specified, so we're ok */
	else {
		size_t min, max;
		ptmax++;
		for(j=0;j+ptmin+1 < ptmax;j++)
			if(!isdigit(ptmin[j])) 
				return CL_SUCCESS;/* not numbers, not functionality level */
		for(j=0;j<strlen(ptmax);j++)
			if(!isdigit(ptmax[j])) 
				return CL_SUCCESS;/* see above */
		ptmax[-1]='\0';
		min = atoi(ptmin);
		if(strlen(ptmax)==0)
 			max = INT_MAX; 		
		else
			max = atoi(ptmax);

		if(min > cl_retflevel()) {
			cli_dbgmsg("regex list line %s not loaded (required f-level: %u)\n",line,(unsigned int)min);
			return CL_EMALFDB; 
		}

		if(max < cl_retflevel()) 
			return CL_EMALFDB;
		ptmin[-1]='\0';
		return CL_SUCCESS;
	}
}

static int add_hash(struct regex_matcher *matcher, char* pattern, const char fl)
{
	int rc;
	struct cli_bm_patt *pat = cli_calloc(1, sizeof(*pat));
	if(!pat)
		return CL_EMEM;
	pat->pattern = (unsigned char*)cli_hex2str(pattern);
	if(!pat->pattern)
		return CL_EMALFDB;
	pat->length = 16;
	pat->virname = cli_malloc(1);
	if(!pat->virname) {
		free(pat);
		return CL_EMEM;
	}
	*pat->virname = fl;
	SO_preprocess_add(&matcher->md5_filter, pat->pattern, pat->length);
	if((rc = cli_bm_addpatt(&matcher->md5_hashes, pat))) {
		cli_errmsg("add_hash: failed to add BM pattern\n");
		free(pat->pattern);
		free(pat->virname);
		free(pat);
		return CL_EMALFDB;
	}
	return CL_SUCCESS;
}


/* Load patterns/regexes from file */
int load_regex_matcher(struct regex_matcher* matcher,FILE* fd,unsigned int options,int is_whitelist,struct cli_dbio *dbio)
{
	int rc,line=0;
	char buffer[FILEBUFF];

	assert(matcher);

	if(matcher->list_inited==-1)
		return CL_EMALFDB; /* already failed to load */
	if(!fd && !dbio) {
		cli_errmsg("Unable to load regex list (null file)\n");
		return CL_EIO;
	}

	cli_dbgmsg("Loading regex_list\n");
	if(!matcher->list_inited) {
		rc = init_regex_list(matcher);
		if (!matcher->list_inited) {
			cli_errmsg("Regex list failed to initialize!\n");
			fatal_error(matcher);
			return rc;
		}
	}
	/*
	 * Regexlist db format (common to .wdb(whitelist) and .pdb(domainlist) files:
	 * Multiple lines of form, (empty lines are skipped):
 	 * Flags RealURL DisplayedURL
	 * Where:
	 * Flags: 
	 *
	 * .pdb files:
	 * R - regex, H - host-only, followed by (optional) 3-digit hexnumber representing 
	 * flags that should be filtered.
	 * [i.e. phishcheck urls.flags that we don't want to be done for this particular host]
	 * 
	 * .wdb files:
	 * X - full URL regex 
	 * Y - host-only regex
	 * M - host simple pattern
	 *
	 * If a line in the file doesn't conform to this format, loading fails
	 * 
	 */
	while(cli_dbgets(buffer, FILEBUFF, fd, dbio)) {
		char* pattern;
		char* flags;
		size_t pattern_len;

		cli_chomp(buffer);
		if(!*buffer)
			continue;/* skip empty lines */

		if(functionality_level_check(buffer))
			continue;

		line++;
		pattern = strchr(buffer,':');
		if(!pattern) {
			cli_errmsg("Malformed regex list line %d\n",line);
			fatal_error(matcher);
			return CL_EMALFDB;
		}
		/*pattern[0]='\0';*/
		flags = buffer+1;
		pattern++;

		pattern_len = strlen(pattern);
		if(pattern_len < FILEBUFF) {
			pattern[pattern_len] = '/';
			pattern[pattern_len+1] = '\0';
		}
		else {
			cli_errmsg("Overlong regex line %d\n",line);
			fatal_error(matcher);
			return CL_EMALFDB;
		}

		if((buffer[0] == 'R' && !is_whitelist) || ((buffer[0] == 'X' || buffer[0] == 'Y') && is_whitelist)) {
			/* regex for hostname*/
			if (( rc = regex_list_add_pattern(matcher, pattern) ))
				return rc==CL_EMEM ? CL_EMEM : CL_EMALFDB;
		}
		else if( ( buffer[0] == 'H' && !is_whitelist) || (buffer[0] == 'M' && is_whitelist)) {
			/*matches displayed host*/
			if (( rc = add_static_pattern(matcher, pattern) ))
				return rc==CL_EMEM ? CL_EMEM : CL_EMALFDB;
		} else if (buffer[0] == 'U' && !is_whitelist) {
			pattern[pattern_len] = '\0';
			if (( rc = add_hash(matcher, pattern, flags[0]) )) {
				cli_errmsg("Error loading at line: %d\n", line);
				return rc==CL_EMEM ? CL_EMEM : CL_EMALFDB;
			}
		} else {
			return CL_EMALFDB;
		}
	}
	matcher->list_loaded = 1;

	return CL_SUCCESS;
}


/* Build the matcher list */
int cli_build_regex_list(struct regex_matcher* matcher)
{
	int rc;
	if(!matcher)
		return CL_SUCCESS;
	if(!matcher->list_inited || !matcher->list_loaded) {
		cli_errmsg("Regex list not loaded!\n");
		return -1;/*TODO: better error code */
	}
	cli_dbgmsg("Building regex list\n");
	hashtab_free(&matcher->suffix_hash);
	if(( rc = cli_ac_buildtrie(&matcher->suffixes) ))
		return rc;
	matcher->list_built=1;

	return CL_SUCCESS;
}

/* Done with this matcher, free resources */
void regex_list_done(struct regex_matcher* matcher)
{
	assert(matcher);

	if(matcher->list_inited) {
		size_t i;
		cli_ac_free(&matcher->suffixes);
		if(matcher->suffix_regexes) {
			for(i=0;i<matcher->suffix_cnt;i++) {
				struct regex_list *r = matcher->suffix_regexes[i].head;
				while(r) {
					struct regex_list *q = r;
					r = r->nxt;
					free(q->pattern);
					free(q);
				}
			}
			free(matcher->suffix_regexes);
			matcher->suffix_regexes = NULL;
		}
		if(matcher->all_pregs) {
			for(i=0;i<matcher->regex_cnt;i++) {
				regex_t *r = matcher->all_pregs[i];
				cli_regfree(r);
				free(r);
			}
			free(matcher->all_pregs);
		}
		hashtab_free(&matcher->suffix_hash);
		cli_bm_free(&matcher->md5_hashes);
		matcher->list_built=0;
		matcher->list_loaded=0;
	}
	if(matcher->list_inited) {
		matcher->list_inited=0;
	}
}

int is_regex_ok(struct regex_matcher* matcher)
{
	assert(matcher);
	return (!matcher->list_inited || matcher->list_inited!=-1);/* either we don't have a regexlist, or we initialized it successfully */
}

static int add_newsuffix(struct regex_matcher *matcher, struct regex_list *info, const char *suffix, size_t len)
{
	struct cli_matcher *root = &matcher->suffixes;
	struct cli_ac_patt *new = cli_calloc(1,sizeof(*new));
	size_t i;
	int ret;

	if(!new)
		return CL_EMEM;
	assert(root && suffix);

	new->rtype = 0;
	new->type = 0;
	new->sigid = 0;
	new->parts = 0;
	new->partno = 0;
	new->mindist = 0;
	new->maxdist = 0;
	new->offset = 0;
	new->length = len;

	new->ch[0] = new->ch[1] |= CLI_MATCH_IGNORE;
	if(new->length > root->maxpatlen)
		root->maxpatlen = new->length;

	new->pattern = cli_malloc(sizeof(new->pattern[0])*len);
	if(!new->pattern) {
		free(new);
		return CL_EMEM;
	}
	for(i=0;i<len;i++)
		new->pattern[i] = suffix[i];/*new->pattern is short int* */

	new->customdata = info;
	new->virname = NULL;
	if((ret = cli_ac_addpatt(root,new))) {
		free(new->pattern);
		free(new);
		return ret;
	}
	SO_preprocess_add(&matcher->filter, (const unsigned char*)suffix, len);
	return CL_SUCCESS;
}

#define MODULE "regex_list: "
/* ------ load a regex, determine suffix, determine suffix2regexlist map ---- */

static void list_add_tail(struct regex_list_ht *ht, struct regex_list *regex)
{
	if(!ht->head)
		ht->head = regex;
	if(ht->tail) {
		ht->tail->nxt = regex;
	}
	ht->tail = regex;
}

/* returns 0 on success, clamav error code otherwise */
static int add_pattern_suffix(void *cbdata, const char *suffix, size_t suffix_len, const struct regex_list *iregex)
{
	struct regex_matcher *matcher = cbdata;
	struct regex_list *regex = cli_malloc(sizeof(*regex));
	const struct element *el;

	assert(matcher);
	if(!regex)
		return CL_EMEM;
	regex->pattern = iregex->pattern ? cli_strdup(iregex->pattern) : NULL;
	regex->preg = iregex->preg;
	regex->nxt = NULL;
	el = hashtab_find(&matcher->suffix_hash, suffix, suffix_len);
	/* TODO: what if suffixes are prefixes of eachother and only one will
	 * match? */
	if(el) {
		/* existing suffix */
		assert((size_t)el->data < matcher->suffix_cnt);
		list_add_tail(&matcher->suffix_regexes[el->data], regex);
		cli_dbgmsg(MODULE "added new regex to existing suffix %s: %s\n", suffix, regex->pattern);
	} else {
		/* new suffix */
		size_t n = matcher->suffix_cnt++;
		el = hashtab_insert(&matcher->suffix_hash, suffix, suffix_len, n);
		matcher->suffix_regexes = cli_realloc(matcher->suffix_regexes, (n+1)*sizeof(*matcher->suffix_regexes));
		if(!matcher->suffix_regexes)
			return CL_EMEM;
		matcher->suffix_regexes[n].tail = regex;
		matcher->suffix_regexes[n].head = regex;
		add_newsuffix(matcher, regex, suffix, suffix_len);
		cli_dbgmsg(MODULE "added new suffix %s, for regex: %s\n", suffix, regex->pattern);
	}
	return 0;
}

static size_t reverse_string(char *pattern)
{
	size_t len = strlen(pattern);
	size_t i;
	for(i=0; i < (len/2); i++) {
		char aux = pattern[i];
		pattern[i] = pattern[len-i-1];
		pattern[len-i-1] = aux;
	}
	return len;
}

static regex_t *new_preg(struct regex_matcher *matcher)
{
	regex_t *r;
	matcher->all_pregs = cli_realloc(matcher->all_pregs, ++matcher->regex_cnt * sizeof(*matcher->all_pregs));
	if(!matcher->all_pregs)
		return NULL;
	r = cli_malloc(sizeof(*r));
	if(!r)
		return NULL;
	matcher->all_pregs[matcher->regex_cnt-1] = r;
	return r;
}

static int add_static_pattern(struct regex_matcher *matcher, char* pattern)
{
	size_t len;
	struct regex_list regex;
	int rc;

	len = reverse_string(pattern);
	regex.nxt = NULL;
	regex.pattern = cli_strdup(pattern);
	regex.preg = NULL;
	rc = add_pattern_suffix(matcher, pattern, len, &regex);
	free(regex.pattern);
	return rc;
}

int regex_list_add_pattern(struct regex_matcher *matcher, char *pattern)
{
	int rc;
	regex_t *preg;
	size_t len;
	/* we only match the host, so remove useless stuff */
	const char remove_end[] = "([/?].*)?/";
	const char remove_end2[] = "([/?].*)/";

	len = strlen(pattern);
	if(len > sizeof(remove_end)) {
		if(strncmp(&pattern[len - sizeof(remove_end)+1], remove_end, sizeof(remove_end)-1) == 0) {
			len -= sizeof(remove_end) - 1;
			pattern[len++]='/';
		}
		if(strncmp(&pattern[len - sizeof(remove_end2)+1], remove_end2, sizeof(remove_end2)-1) == 0) {
			len -= sizeof(remove_end2) - 1;
			pattern[len++]='/';
		}
	}
	pattern[len] = '\0';

	preg = new_preg(matcher);
	if(!preg)
		return CL_EMEM;

	rc = cli_regex2suffix(pattern, preg, add_pattern_suffix, (void*)matcher);
	if(rc) {
		cli_regfree(preg);
	}

	return rc;
}
