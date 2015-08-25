/**
This software is released under the terms of the Apache License version 2.
For details of the license, see http://www.apache.org/licenses/LICENSE-2.0.
*/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

#include "libs3.h"
#include "libs3uri.h"

#define LS3_URIDEBUG

#ifdef LS3_URIDEBUG
static int failpoint = 0;
#define THROW(n) {failpoint=(n); goto fail;}
#else
#define THROW(n) {goto fail;}
#endif

#define PADDING 8

#define LBRACKET '['
#define RBRACKET ']'
#define EOFCHAR '\0'

#define NILFIX(s) ((s)==NULL?"NULL":(s))

#define NILLEN(s) ((s)==NULL?0:strlen(s))

#define nulldup(s) ((s)==NULL?NULL:strdup(s))

#define terminate(p) {*(p) = EOFCHAR;}

#define endof(p) ((p)+strlen(p))

static struct ProtocolInfo {
char* name;
int   filelike; /* 1=>this protocol has no host, user+pwd, or port */
} legalprotocols[] = {
{"file",1},
{"http",0},
{"https",0},
{"s3",0},
{"s3s",0},
};

/* Allowable character sets for encode */
static char* fileallow =
"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!#$&'()*+,-./:;=?@_~";

static char* queryallow =
"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ!#$&'()*+,-./:;=?@_~";

/* Not all systems have strndup, so provide one*/
char*
ls3_strndup(const char* s, size_t len)
{
    char* dup;
    if(s == NULL) return NULL;
    dup = (char*)malloc(len+1);
    if(dup == NULL) return NULL;
    memcpy((void*)dup,s,len);
    dup[len] = '\0';
    return dup;
}

/* Forward */
static void s3paramfree(char** params);
static int s3find(char** params, const char* key);
static void s3lshift1(char* p);
static void s3rshift1(char* p);
static char* s3locate(char* p, const char* charlist);
static void s3appendparams(char* newuri, char** p);

/* Do a simple uri parse: return 0 if fail, 1 otherwise*/
int
ls3_uriparse(const char* uri0, LS3URI** durip)
{
    LS3URI* duri = NULL;
    char* uri = NULL;
    char* p;
    struct ProtocolInfo* proto;
    int i,nprotos;

    /* accumulate parse points*/
    char* protocol = NULL;
    char* host = NULL;
    char* port = NULL;
    char* constraint = NULL;
    char* user = NULL;
    char* pwd = NULL;
    char* file = NULL;
    char* prefixparams = NULL;
    char* suffixparams = NULL;

    if(uri0 == NULL || strlen(uri0) == 0)
	{THROW(1);}

    duri = (LS3URI*)calloc(1,sizeof(LS3URI));
    if(duri == NULL)
      {THROW(2);}

    /* save original uri */
    duri->uri = nulldup(uri0);

    /* make local copy of uri */
    uri = (char*)malloc(strlen(uri0)+1+PADDING); /* +1 for trailing null,
                                                    +PADDING for shifting */
    if(uri == NULL)
	{THROW(3);}

    /* strings will be broken into pieces with intermixed '\0; characters;
       first char is guaranteed to be '\0' */

    duri->strings = uri;
    uri++;

    /* dup the incoming url */
    strcpy(uri,uri0);

    /* Walk the uri and do the following:
	1. remove all whitespace
	2. remove all '\\' (Temp hack to remove escape characters
                            inserted by Windows or MinGW)
    */
    for(p=uri;*p;p++) {
	if(*p == '\\' || *p < ' ')
	    nclshift1(p); /* compress out */
    }

    p = uri;

    /* break up the uri string into big chunks: prefixparams, protocol,
       host section, and the file section (i.e. remainder)
    */

    /* collect any prefix bracketed parameters */
    if(*p == LBRACKET) {
	prefixparams = p+1;
	/* find end of the clientparams; convert LB,RB to '&' */
        for(;*p;p++) {
	    if(p[0] == RBRACKET && p[1] == LBRACKET) {
		p[0] = '&';
		nclshift1(p+1);
	    } else if(p[0] == RBRACKET && p[1] != LBRACKET)
		break;
	}
	if(*p == 0)
	    {THROW(4); /* malformed client params*/}
        terminate(p); /* nul term the prefixparams (overwrites
                         the final RBRACKET) */
	p++; /* move past the final RBRACKET */
    }

    /* Tag the protocol */
    protocol = p;
    p = strchr(p,':');
    if(!p)
	{THROW(5);}
    terminate(p); /*overwrite colon*/
    p++; /* skip the colon */

    /* verify that the uri starts with an acceptable protocol*/
    nprotos = (sizeof(legalprotocols)/sizeof(struct ProtocolInfo));
    proto = NULL;
    for(i=0;i<nprotos;i++) {
        if(strcmp(protocol,legalprotocols[i].name)==0) {
	    proto = &legalprotocols[i];
	    break;
	}
    }
    if(proto == NULL)
	{THROW(6); /* illegal protocol*/}

    /* skip // */
    if(p[0] != '/' || p[1] != '/')
	{THROW(7);}
    p += 2;

    /* If this is all we have (proto://) then fail */
    if(*p == EOFCHAR)
	{THROW(8);}

    /* establish the start of the file section */
    if(proto->filelike) {/* everything after proto:// */
	file = p;
	host = NULL; /* and no host section */
    } else { /*!proto->filelike => This means there should be a host section */
        /* locate the end of the host section and therefore the start
           of the file section */
	host = p;
        p  = s3locate(p,"/?#");
	if(p == NULL) {
	    file = endof(host); /* there is no file section */
	} else {
	    ncrshift1(p); /* make room to terminate the host section
                             without overwriting the leading character */
	    terminate(p); /* terminate the host section */
	    file = p+1; /* +1 becauseof the shift */
	}
    }

    /* If you shift in the code below, you must reset file beginning */

    if(host != NULL) {/* Parse the host section */
	/* Check for leading user:pwd@ */
        p = strchr(host,'@');
        if(p) {
	    if(p == host)
		{THROW(9); /* we have proto://@ */}
	    user = host;
	    terminate(p); /* overwrite '@' */
	    host = p+1; /* start of host ip name */
	    p = strchr(user,':');
 	    if(p == NULL)
		{THROW(10); /* malformed */}
	    terminate(p); /*overwrite colon */
	    pwd = p+1;
	}

        /* extract host and port */
	p = host;
        p = strchr(p,':');
        if(p != NULL) {
	    terminate(p);
	    p++;
	    port = p;
	    if(*port == EOFCHAR)
		{THROW(11); /* we have proto://...:/ */}
	    /* The port must look something like a number */
	    for(;*p;p++) {
	        if(strchr("0123456789-",*p) == NULL)
		    {THROW(12);  /* probably not a real port, fail */}
	    }
	} /* else *p == NULL */


        /* check for empty host section */
	if(*host == EOFCHAR)
	    {THROW(13);}

    }

    assert(file != NULL);
    p = file;

    /* find the end of the file section and the start of the
       constraints and/or suffixparams
    */
    p = s3locate(p,"?#");
    if(p != NULL) { /* we have constraint and/or suffixparams */
	char* fileend = p; /* save the end of the file section */
	char* constraintend = NULL;
	if(*p == '?')
            constraint = p+1;
	else
	    constraint = NULL;
	p = strchr(p,'#'); /* may repeat effect of nclocate above */
	if(p != NULL) {
	    constraintend = p;
	    suffixparams = p+1;
	} else
	    suffixparams = NULL;
	/* Ok, terminate the pieces */
	terminate(fileend); /* terminate file section */
	if(constraint != NULL && constraintend != NULL)
	    terminate(constraintend);
	/* Suffix params are already terminated
           since they should be the last section
           of the original url
        */
    }

    /* check for empty sections */
    if(file != NULL && *file == EOFCHAR)
	file = NULL; /* empty file section */
    if(constraint != NULL && *constraint == EOFCHAR)
	constraint = NULL; /* empty constraint section */
    if(suffixparams != NULL && *suffixparams == EOFCHAR)
	suffixparams = NULL; /* empty suffixparams section */

    if(suffixparams != NULL) {
	/* there really are suffix params; so rebuild the suffix params */
	if(*suffixparams == LBRACKET) suffixparams++;
        p = suffixparams;
	/* convert RBRACKET LBRACKET to '&' */
        for(;*p;p++) {
	    if(p[0] == RBRACKET && p[1] == LBRACKET) {
	        p[0] = '&';
		nclshift1(p+1);
	    } else if(p[0] == RBRACKET && p[1] != LBRACKET) {
		/* terminate suffixparams */
		*p = EOFCHAR;
		break;
	    }
	}
	if(*suffixparams == EOFCHAR)
	    suffixparams = NULL; /* suffixparams are empty */
    }

    /* do last minute empty check */

    if(*protocol == EOFCHAR) protocol = NULL;
    if(user != NULL && *user == EOFCHAR) user = NULL;
    if(pwd != NULL && *pwd == EOFCHAR) pwd = NULL;
    if(host != NULL && *host == EOFCHAR) host = NULL;
    if(port != NULL && *port == EOFCHAR) port = NULL;
    if(file != NULL && *file == EOFCHAR) file = NULL;
    if(constraint != NULL && *constraint == EOFCHAR) constraint = NULL;

    /* assemble the component pieces */
    duri->protocol = protocol;
    duri->user = user;
    duri->password = pwd;
    duri->host = host;
    duri->port = port;
    duri->file = file;

    ls3_urisetconstraints(duri,constraint);

    /* concat suffix and prefix params */
    if(prefixparams != NULL || suffixparams != NULL) {
	int plen = prefixparams ? strlen(prefixparams) : 0;
	int slen = suffixparams ? strlen(suffixparams) : 0;
	int space = plen + slen + 1;
	/* add 1 for an extra ampersand if both are defined */
        space++;
        duri->params = (char*)malloc(space);
	duri->params[0] = EOFCHAR; /* so we can use strcat */
	if(plen > 0) {
            strcat(duri->params,prefixparams);
	    if(slen > 0)
		strcat(duri->params,"&");
	}
	if(slen > 0)
            strcat(duri->params,suffixparams);
    }

#ifdef NCXDEBUG
	{
        fprintf(stderr,"duri:");
        fprintf(stderr," params=|%s|",NILFIX(duri->params));
        fprintf(stderr," protocol=|%s|",NILFIX(duri->protocol));
        fprintf(stderr," host=|%s|",NILFIX(duri->host));
        fprintf(stderr," port=|%s|",NILFIX(duri->port));
        fprintf(stderr," file=|%s|",NILFIX(duri->file));
        fprintf(stderr," constraint=|%s|",NILFIX(duri->constraint));
        fprintf(stderr,"\n");
    }
#endif
    if(durip != NULL)
      *durip = duri;
    else
      ls3_urifree(duri);

    return 1;

fail:
    if(duri != NULL) {
	ls3_urifree(duri);
    }
    return 0;
}

void
ls3_urifree(LS3URI* duri)
{
    if(duri == NULL) return;
    if(duri->uri != NULL) {free(duri->uri);}
    if(duri->params != NULL) {free(duri->params);}
    if(duri->paramlist != NULL) ncparamfree(duri->paramlist);
    if(duri->strings != NULL) {free(duri->strings);}
    if(duri->constraint != NULL) {free(duri->constraint);}
    if(duri->projection != NULL) {free(duri->projection);}
    if(duri->selection != NULL) {free(duri->selection);}
    free(duri);
}

/* Replace the constraints */
void
ls3_urisetconstraints(LS3URI* duri,const char* constraints)
{
    char* proj = NULL;
    char* select = NULL;
    const char* p;

    if(duri->constraint != NULL) free(duri->constraint);
    if(duri->projection != NULL) free(duri->projection);
    if(duri->selection != NULL) free(duri->selection);
    duri->constraint = NULL;
    duri->projection = NULL;
    duri->selection = NULL;

    if(constraints == NULL || strlen(constraints)==0) return;

    duri->constraint = nulldup(constraints);
    if(*duri->constraint == '?')
	nclshift1(duri->constraint);

    p = duri->constraint;
    proj = (char*) p;
    select = strchr(proj,'&');
    if(select != NULL) {
        size_t plen = (size_t)(select - proj);
	if(plen == 0) {
	    proj = NULL;
	} else {
	    proj = (char*)malloc(plen+1);
	    memcpy((void*)proj,p,plen);
	    proj[plen] = EOFCHAR;
	}
	select = nulldup(select);
    } else {
	proj = nulldup(proj);
	select = NULL;
    }
    duri->projection = proj;
    duri->selection = select;
}


/* Construct a complete NC URI.
   Optionally with the constraints.
   Optionally with the user parameters.
   Caller frees returned string.
   Optionally encode the pieces.
*/

char*
ls3_uribuild(LS3URI* duri, const char* prefix, const char* suffix, int flags)
{
    size_t len = 0;
    char* newuri;
    char* tmpfile;
    char* tmpsuffix;
    char* tmpquery;
    size_t nparams = 0;
    size_t paramslen = 0;

    /* if both are specified, prefix has priority */
    int withsuffixparams = ((flags&S3URISUFFIXPARAMS)!=0
				&& duri->params != NULL);
    int withprefixparams = ((flags&S3URIPREFIXPARAMS)!=0
				&& duri->params != NULL);
    int withuserpwd = ((flags&S3URIUSERPWD)!=0
	               && duri->user != NULL && duri->password != NULL);
    int withconstraints = ((flags&S3URICONSTRAINTS)!=0
	                   && duri->constraint != NULL);
#ifdef NEWESCAPE
    const int encode = (flags&S3URIENCODE);
#else
    const int encode = 0;
#endif

    if(prefix != NULL) len += NILLEN(prefix);
    len += (NILLEN(duri->protocol)+NILLEN("://"));
    if(withuserpwd) {
	len += (NILLEN(duri->user)+NILLEN(duri->password)+NILLEN(":@"));
    }
    len += (NILLEN(duri->host));
    if(duri->port != NULL) {
	len += (NILLEN(":")+NILLEN(duri->port));
    }

    tmpfile = duri->file;
    if(encode)
	tmpfile = ls3_uriencode(tmpfile,fileallow);
    len += (NILLEN(tmpfile));

    if(suffix != NULL) {
        tmpsuffix = (char*)suffix;
        if(encode)
	    tmpsuffix = ls3_uriencode(tmpsuffix,fileallow);
        len += (NILLEN(tmpsuffix));
    }

    if(withconstraints) {
	tmpquery = duri->constraint;
        if(encode)
	    tmpquery = ls3_uriencode(tmpquery,queryallow);
        len += (NILLEN("?")+NILLEN(tmpquery));
    }

    if(withprefixparams || withsuffixparams) {
	char** p;
	if(duri->paramlist == NULL)
	    if(!ls3_uridecodeparams(duri))
		return NULL;
	for(paramslen=0,nparams=0,p=duri->paramlist;*p;p++) {
	    nparams++;
	    paramslen += NILLEN(*p);
	}
	if(nparams % 2 == 1)
	    return NULL; /* malformed */
	nparams = (nparams / 2);
	len += paramslen;
	len += 3*nparams; /* for brackets for every param plus possible = */
	if(withsuffixparams)
	    len += strlen("#");
    }

    len += 1; /* null terminator */

    newuri = (char*)malloc(len);
    if(newuri == NULL) return NULL;

    newuri[0] = EOFCHAR;
    if(prefix != NULL) strcat(newuri,prefix);
    if(withprefixparams) {
	ncappendparams(newuri,duri->paramlist);
    }
    if(duri->protocol != NULL)
	strcat(newuri,duri->protocol);
    strcat(newuri,"://");
    if(withuserpwd) {
        strcat(newuri,duri->user);
        strcat(newuri,":");
        strcat(newuri,duri->password);
        strcat(newuri,"@");
    }
    if(duri->host != NULL) { /* may be null if using file: protocol */
        strcat(newuri,duri->host);
    }
    if(duri->port != NULL) {
        strcat(newuri,":");
        strcat(newuri,duri->port);
    }

    if(tmpfile != NULL) {
        strcat(newuri,tmpfile);
        if(suffix != NULL) strcat(newuri,tmpsuffix);
    }
    if(withconstraints) {
	strcat(newuri,"?");
	strcat(newuri,tmpquery);
    }
    if(withsuffixparams & !withprefixparams) {
	strcat(newuri,"#");
	ncappendparams(newuri,duri->paramlist);
    }
    return newuri;
}

static void
s3appendparams(char* newuri, char** p)
{
	while(*p) {
	    strcat(newuri,"[");
	    strcat(newuri,*p++);
	    if(strlen(*p) > 0) {
	        strcat(newuri,"=");
	        strcat(newuri,*p);
	    }
	    p++;
	    strcat(newuri,"]");
	}
}

/**************************************************/
/* Parameter support */

/*
In the original url, client parameters are assumed to be one
or more instances of bracketed pairs: e.g "[...][...]...".
They may occur either at the front, or suffixed after
a trailing # character. After processing, the list is
converted to an ampersand separated list of the combination
of prefix and suffix parameters.

After the url is parsed, the parameter list
is converted to an ampersand separated list with all
whitespace removed.
In any case, each parameter in turn is assumed to be a
of the form <name>=<value> or [<name>].
e.g. [x=y][z][a=b][w].  If the same parameter is specified more
than once, then the first occurrence is used; this is so
that is possible to forcibly override user specified
parameters by prefixing.  IMPORTANT: client parameter string
is assumed to have blanks compressed out.  Returns 1 if parse
suceeded, 0 otherwise; */

int
ls3_uridecodeparams(LS3URI* ls3uri)
{
    char* cp = NULL;
    int i,c;
    size_t nparams;
    char* params = NULL;
    char** plist;

    if(ls3uri == NULL) return 0;
    if(ls3uri->params == NULL) return 1;

    params = ls3_strndup(ls3uri->params,
		     (strlen(ls3uri->params)+1)); /* so we can modify */
    if(!params)
      return S3_ENOMEM;

    /* Pass 1 to break string into pieces at the ampersands
       and count # of pairs */
    nparams=0;
    for(cp=params;(c=*cp);cp++) {
	if(c == '&') {*cp = EOFCHAR; nparams++;}
    }
    nparams++; /* for last one */

    /* plist is an env style list */
    plist = (char**)calloc(1,sizeof(char*)*(2*nparams+1)); /* +1 for null termination */
    if(plist == NULL) {
      if(params) free(params);
      return 0;
    }

    /* Break up each param into a (name,value) pair*/
    /* and insert into the param list */
    /* parameters of the form name name= are converted to name=""*/
    for(cp=params,i=0;i<nparams;i++) {
	char* next = cp+strlen(cp)+1; /* save ptr to next pair*/
	char* vp;
	/*break up the ith param*/
	vp = strchr(cp,'=');
	if(vp != NULL) {*vp = EOFCHAR; vp++;} else {vp = "";}
	plist[2*i] = nulldup(cp);
	plist[2*i+1] = nulldup(vp);
	cp = next;
    }
    plist[2*nparams] = NULL;
    free(params);
    if(ls3uri->paramlist != NULL)
	ncparamfree(ls3uri->paramlist);
    ls3uri->paramlist = plist;
    return 1;
}

int
ls3_urilookup(LS3URI* uri, const char* key, const char** resultp)
{
    int i;
    char* value = NULL;
    if(uri == NULL || key == NULL || uri->params == NULL) return 0;
    if(uri->paramlist == NULL) {
	i = ls3_uridecodeparams(uri);
	if(!i) return 0;
    }
    /* Coverity[FORWARD_NULL] */
    i = ncfind(uri->paramlist,key);
    if(i < 0)
	return 0;
    value = uri->paramlist[(2*i)+1];
    if(resultp) *resultp = value;
    return 1;
}

int
ls3_urisetparams(LS3URI* uri, const char* newparams)
{
    if(uri == NULL) return 0;
    if(uri->paramlist != NULL) ncparamfree(uri->paramlist);
    uri->paramlist = NULL;
    if(uri->params != NULL) free(uri->params);
    uri->params = nulldup(newparams);
    return 1;
}

/* Internal version of lookup; returns the paired index of the key */
static int
s3find(char** params, const char* key)
{
    int i;
    char** p;
    for(i=0,p=params;*p;p+=2,i++) {
	if(strcmp(key,*p)==0) return i;
    }
    return -1;
}

static void
s3paramfree(char** params)
{
    char** p;
    if(params == NULL) return;
    for(p=params;*p;p+=2) {
	free(*p);
	if(p[1] != NULL) free(p[1]);
    }
    free(params);
}


/* Return the ptr to the first occurrence of
   any char in the list. Return NULL if no
   occurrences
*/
static char*
s3locate(char* p, const char* charlist)
{
    for(;*p;p++) {
	if(strchr(charlist,*p) != NULL)
	    return p;
    }
    return NULL;
}


/* Shift every char starting at p 1 place to the left */
static void
s3lshift1(char* p)
{
    if(p != NULL && *p != EOFCHAR) {
	char* q = p++;
	while((*q++=*p++));
    }
}

/* Shift every char starting at p 1 place to the right */
static void
s3rshift1(char* p)
{
    char cur;
    cur = 0;
    do {
	char next = *p;
	*p++ = cur;
	cur = next;
    } while(cur != 0);
    *p = 0; /* make sure we are still null terminated */
}


/* Provide % encoders and decoders */


static char* hexchars = "0123456789abcdefABCDEF";

static void
toHex(unsigned int b, char hex[2])
{
    hex[0] = hexchars[(b >> 4) & 0xff];
    hex[1] = hexchars[(b) & 0xff];
}


static int
fromHex(int c)
{
    if(c >= '0' && c <= '9') return (int) (c - '0');
    if(c >= 'a' && c <= 'f') return (int) (10 + (c - 'a'));
    if(c >= 'A' && c <= 'F') return (int) (10 + (c - 'A'));
    return 0;
}


/* Return a string representing encoding of input; caller must free;
   watch out: will encode whole string, so watch what you give it.
   Allowable argument specifies characters that do not need escaping.
 */

char*
ls3_uriencode(char* s, char* allowable)
{
    size_t slen;
    char* encoded;
    char* inptr;
    char* outptr;

    if(s == NULL) return NULL;

    slen = strlen(s);
    encoded = (char*)malloc((3*slen) + 1); /* max possible size */

    for(inptr=s,outptr=encoded;*inptr;) {
	int c = *inptr++;
        if(c == ' ') {
	    *outptr++ = '+';
        } else {
            /* search allowable */
            int c2;
	    char* a = allowable;
	    while((c2=*a++)) {
		if(c == c2) break;
	    }
            if(c2) {*outptr++ = (char)c;}
            else {
		char hex[2];
		toHex(c,hex);
		*outptr++ = '%';
		*outptr++ = hex[0];
		*outptr++ = hex[1];
            }
        }
    }
    *outptr = EOFCHAR;
    return encoded;
}

/* Return a string representing decoding of input; caller must free;*/
char*
ls3_uridecode(char* s)
{
    return ls3_uridecodeonly(s,NULL);
}

/* Return a string representing decoding of input only for specified
   characters;  caller must free
*/
char*
ls3_uridecodeonly(char* s, char* only)
{
    size_t slen;
    char* decoded;
    char* outptr;
    char* inptr;
    unsigned int c;

    if (s == NULL) return NULL;

    slen = strlen(s);
    decoded = (char*)malloc(slen+1); /* Should be max we need */

    outptr = decoded;
    inptr = s;
    while((c = (unsigned int)*inptr++)) {
	if(c == '+' && only != NULL && strchr(only,'+') != NULL)
	    *outptr++ = ' ';
	else if(c == '%') {
            /* try to pull two hex more characters */
	    if(inptr[0] != EOFCHAR && inptr[1] != EOFCHAR
		&& strchr(hexchars,inptr[0]) != NULL
		&& strchr(hexchars,inptr[1]) != NULL) {
		/* test conversion */
		int xc = (fromHex(inptr[0]) << 4) | (fromHex(inptr[1]));
		if(only == NULL || strchr(only,xc) != NULL) {
		    inptr += 2; /* decode it */
		    c = (unsigned int)xc;
                }
            }
        }
        *outptr++ = (char)c;
    }
    *outptr = EOFCHAR;
    return decoded;
}