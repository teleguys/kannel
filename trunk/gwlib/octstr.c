/*
 * octstr.c - implementation of Octet strings
 *
 * See octstr.h for explanations of what public functions should do.
 *
 * Lars Wirzenius
 */


#include <ctype.h>
#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "gwlib.h"

/* 
 * Unfortunately some platforms base va_list an an array type
 * which makes passing of the &args a bit tricky 
 */
#if defined(__linux__) && (defined(__powerpc__) || defined(__s390__))
#define VARGS(x)   (x)
#define VALPARM(y) va_list y
#define VALST(z)   (z)
#else
#define VARGS(x)   (&x)
#define VALPARM(y) va_list *y
#define VALST(z)   (*z)
#endif


/***********************************************************************
 * Definitions of data structures. These are not visible to the external
 * world -- they may be accessed only via the functions declared in
 * octstr.h. This ensures they really are abstract.
 */

/*
 * The octet string.
 *
 * `data' is a pointer to dynamically allocated memory are where the 
 * octets in the string. It may be bigger than the actual length of the
 * string.
 *
 * `len' is the length of the string.
 *
 * `size' is the size of the memory area `data' points at.
 *
 * When `size' is greater than zero, it is at least `len+1', and the
 * character at `len' is '\0'. This is so that octstr_get_cstr will
 * always work.
 *
 * `immutable' defines whether the octet string is immutable or not.
 */
struct Octstr
{
    unsigned char *data;
    long len;
    long size;
    int immutable;
};


/**********************************************************************
 * Hash table of immutable octet strings.
 */

#define MAX_IMMUTABLES 1024

static Octstr *immutables[MAX_IMMUTABLES];
static Mutex immutables_mutex;
static int immutables_init = 0;

static char is_safe[UCHAR_MAX + 1];

/*
 * Convert a pointer to a C string literal to a long that can be used
 * for hashing. This is done by converting the pointer into an integer
 * and discarding the lowest to bits to get rid of typical alignment
 * bits.
 */
#define CSTR_TO_LONG(ptr)	(((long) ptr) >> 2)

/***********************************************************************
 * Declarations of internal functions. These are defined at the end of
 * the file.
 */


static void seems_valid_real(Octstr *ostr, const char *filename, long lineno,
                             const char *function);
#ifdef NO_GWASSERT
#define seems_valid(ostr)
#else
#define seems_valid(ostr) \
    (seems_valid_real(ostr, __FILE__, __LINE__, __func__))
#endif


/***********************************************************************
 * Implementations of the functions declared in octstr.h. See the
 * header for explanations of what they should do.
 */


/* Reserve space for at least 'size' octets */
static void octstr_grow(Octstr *ostr, long size)
{
    gw_assert(!ostr->immutable);
    seems_valid(ostr);
    gw_assert(size >= 0);

    size++;   /* make room for the invisible terminating NUL */

    if (size > ostr->size) {
        ostr->data = gw_realloc(ostr->data, size);
        ostr->size = size;
    }
}


/*
 * Fill is_safe table. is_safe[c] means that c can be left as such when
 * url-encoded.
 * RFC 2396 defines the list of characters that need to be encoded.
 * Space is treated as an exception by the encoding routine;
 * it's listed as safe here, but is actually changed to '+'.
 */
static void urlcode_init(void)
{
    int i;

    unsigned char *safe = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
	"abcdefghijklmnopqrstuvwxyz-_.!~*'()";
    for (i = 0; safe[i] != '\0'; ++i)
	is_safe[safe[i]] = 1;
}


void octstr_init(void)
{
    urlcode_init();
    mutex_init_static(&immutables_mutex);
    immutables_init = 1;
}


void octstr_shutdown(void)
{
    long i, n;

    n = 0;
    for (i = 0; i < MAX_IMMUTABLES; ++i) {
        if (immutables[i] != NULL) {
	    gw_free(immutables[i]);
            ++n;
        }
    }
    if(n>0)
        debug("gwlib.octstr", 0, "Immutable octet strings: %ld.", n);
    mutex_destroy(&immutables_mutex);
}


Octstr *octstr_create_real(const char *cstr)
{
    gw_assert(cstr != NULL);
    return octstr_create_from_data(cstr, strlen(cstr));
}


Octstr *octstr_create_from_data_real(const char *data, long len)
{
    Octstr *ostr;

    gw_assert(len >= 0);
    if (data == NULL)
        gw_assert(len == 0);

    ostr = gw_malloc(sizeof(*ostr));
    if (len == 0) {
        ostr->len = 0;
        ostr->size = 0;
        ostr->data = NULL;
    } else {
        ostr->len = len;
        ostr->size = len + 1;
        ostr->data = gw_malloc(ostr->size);
        memcpy(ostr->data, data, len);
        ostr->data[len] = '\0';
    }
    ostr->immutable = 0;
    seems_valid(ostr);
    return ostr;
}


Octstr *octstr_imm(const char *cstr)
{
    Octstr *os;
    long i, index;
    unsigned char *data;

    gw_assert(immutables_init);
    gw_assert(cstr != NULL);

    index = CSTR_TO_LONG(cstr) % MAX_IMMUTABLES;
    data = (unsigned char *) cstr;

    mutex_lock(&immutables_mutex);
    i = index;
    for (; ; ) {
	if (immutables[i] == NULL || immutables[i]->data == data)
            break;
        i = (i + 1) % MAX_IMMUTABLES;
        if (i == index)
            panic(0, "Too many immutable strings.");
    }
    os = immutables[i];
    if (os == NULL) {
	/*
	 * Can't use octstr_create() because it copies the string,
	 * which would break our hashing.
	 */
	os = gw_malloc(sizeof(*os));
        os->data = data;
        os->len = strlen(data);
        os->size = os->len + 1;
        os->immutable = 1;
	immutables[i] = os;
	seems_valid(os);
    }
    mutex_unlock(&immutables_mutex);

    return os;
}


void octstr_destroy(Octstr *ostr)
{
    if (ostr != NULL) {
        seems_valid(ostr);
	if (!ostr->immutable) {
            gw_free(ostr->data);
            gw_free(ostr);
        }
    }
}


void octstr_destroy_item(void *os)
{
    octstr_destroy(os);
}


long octstr_len(Octstr *ostr)
{
    if (ostr == NULL)
        return 0;
    seems_valid(ostr);
    return ostr->len;
}


Octstr *octstr_copy_real(Octstr *ostr, long from, long len)
{
    seems_valid(ostr);
    gw_assert(from >= 0);
    gw_assert(len >= 0);

    if (from >= ostr->len)
        return octstr_create("");

    if (len > ostr->len - from)
        len = ostr->len - from;

    return octstr_create_from_data(ostr->data + from, len);
}



Octstr *octstr_duplicate_real(Octstr *ostr)
{
    if (ostr == NULL)
        return NULL;
    seems_valid(ostr);
    return octstr_create_from_data(ostr->data, ostr->len);
}


Octstr *octstr_cat(Octstr *ostr1, Octstr *ostr2)
{
    Octstr *ostr;

    seems_valid(ostr1);
    seems_valid(ostr2);
    gw_assert(!ostr1->immutable);

    ostr = octstr_create("");
    ostr->len = ostr1->len + ostr2->len;
    ostr->size = ostr->len + 1;
    ostr->data = gw_malloc(ostr->size);

    if (ostr1->len > 0)
        memcpy(ostr->data, ostr1->data, ostr1->len);
    if (ostr2->len > 0)
        memcpy(ostr->data + ostr1->len, ostr2->data, ostr2->len);
    ostr->data[ostr->len] = '\0';

    seems_valid(ostr);
    return ostr;
}


int octstr_get_char(Octstr *ostr, long pos)
{
    seems_valid(ostr);
    if (pos >= ostr->len || pos < 0)
        return -1;
    return ostr->data[pos];
}


void octstr_set_char(Octstr *ostr, long pos, int ch)
{
    seems_valid(ostr);
    gw_assert(!ostr->immutable);
    if (pos < ostr->len)
        ostr->data[pos] = ch;
    seems_valid(ostr);
}


void octstr_get_many_chars(char *buf, Octstr *ostr, long pos, long len)
{
    gw_assert(buf != NULL);
    seems_valid(ostr);

    if (pos >= ostr->len)
        return;
    if (pos + len > ostr->len)
        len = ostr->len - pos;
    if (len > 0)
        memcpy(buf, ostr->data + pos, len);
}


char *octstr_get_cstr_real(Octstr *ostr, const char *file, long line, 
    	    	    	   const char *func)
{
    if (!ostr)
        return "(null)";
    seems_valid_real(ostr, file, line, func);
    if (ostr->len == 0)
        return "";
    return ostr->data;
}


void octstr_append_from_hex(Octstr *ostr, char *hex)
{
    Octstr *output;
	
    seems_valid(ostr);
    gw_assert(!ostr->immutable);
	
    output = octstr_create(hex);
    octstr_hex_to_binary(output);
    octstr_append(ostr, output);
    octstr_destroy(output);
}


void octstr_binary_to_hex(Octstr *ostr, int uppercase)
{
    unsigned char *hexits;
    long i;

    seems_valid(ostr);
    gw_assert(!ostr->immutable);
    if (ostr->len == 0)
        return;

    hexits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    octstr_grow(ostr, ostr->len * 2);

    /* In-place modification must be done back-to-front to avoid
     * overwriting the data while we read it.  Even the order of
     * the two assignments is important, to get i == 0 right. */
    for (i = ostr->len - 1; i >= 0; i--) {
        ostr->data[i * 2 + 1] = hexits[ostr->data[i] % 16];
        ostr->data[i * 2] = hexits[(ostr->data[i] / 16) & 0xf];
    }

    ostr->len = ostr->len * 2;
    ostr->data[ostr->len] = '\0';

    seems_valid(ostr);
}


int octstr_hex_to_binary(Octstr *ostr)
{
    long len, i;
    unsigned char *p;

    seems_valid(ostr);
    gw_assert(!ostr->immutable);

    if (ostr->len == 0)
        return 0;

    /* Check if it's in the right format */
    if (!octstr_check_range(ostr, 0, ostr->len, gw_isxdigit))
        return -1;

    len = ostr->len;

    /* Convert ascii data to binary values */
    for (i = 0, p = ostr->data; i < len; i++, p++) {
        if (*p >= '0' && *p <= '9')
            *p -= '0';
        else if (*p >= 'a' && *p <= 'f')
            *p = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F')
            *p = *p - 'A' + 10;
        else {
            /* isxdigit checked the whole string, so we should
             * not be able to get here. */
            gw_assert(0);
            *p = 0;
        }
    }

    /* De-hexing will compress data by factor of 2 */
    len = ostr->len / 2;

    for (i = 0; i < len; i++) {
        ostr->data[i] = ostr->data[i * 2] * 16 | ostr->data[i * 2 + 1];
    }

    ostr->len = len;
    ostr->data[len] = '\0';

    seems_valid(ostr);
    return 0;
}


void octstr_binary_to_base64(Octstr *ostr)
{
    static const unsigned char base64[64] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    long triplets;
    long lines;
    long orig_len;
    unsigned char *data;
    long from, to;
    int left_on_line;

    seems_valid(ostr);
    gw_assert(!ostr->immutable);

    if (ostr->len == 0) {
        /* Always terminate with CR LF */
        octstr_insert(ostr, octstr_imm("\015\012"), 0);
        return;
    }

    /* The lines must be 76 characters each (or less), and each
     * triplet will expand to 4 characters, so we can fit 19
     * triplets on one line.  We need a CR LF after each line,
     * which will add 2 octets per 19 triplets (rounded up). */
    triplets = (ostr->len + 2) / 3;   /* round up */
    lines = (triplets + 18) / 19;

    octstr_grow(ostr, triplets * 4 + lines * 2);
    orig_len = ostr->len;
    data = ostr->data;

    ostr->len = triplets * 4 + lines * 2;
    data[ostr->len] = '\0';

    /* This function works back-to-front, so that encoded data will
     * not overwrite source data.
     * from points to the start of the last triplet (which may be
     * an odd-sized one), and to points to the start of where the
     * last quad should go.  */
    from = (triplets - 1) * 3;
    to = (triplets - 1) * 4 + (lines - 1) * 2;

    /* First write the CR LF after the last quad */
    data[to + 5] = 10;   /* LF */
    data[to + 4] = 13;   /* CR */
    left_on_line = triplets - ((lines - 1) * 19);

    /* base64 encoding is in 3-octet units.  To handle leftover
     * octets, conceptually we have to zero-pad up to the next
     * 6-bit unit, and pad with '=' characters for missing 6-bit
     * units.
     * We do it by first completing the first triplet with 
     * zero-octets, and after the loop replacing some of the
     * result characters with '=' characters.
     * There is enough room for this, because even with a 1 or 2
     * octet source string, space for four octets of output
     * will be reserved.
     */
    switch (orig_len % 3) {
    case 0:
        break;
    case 1:
        data[orig_len] = 0;
        data[orig_len + 1] = 0;
        break;
    case 2:
        data[orig_len + 1] = 0;
        break;
    }

    /* Now we only have perfect triplets. */
    while (from >= 0) {
        long whole_triplet;

        /* Add a newline, if necessary */
        if (left_on_line == 0) {
            to -= 2;
            data[to + 5] = 10;  /* LF */
            data[to + 4] = 13;  /* CR */
            left_on_line = 19;
        }

        whole_triplet = (data[from] << 16) |
                        (data[from + 1] << 8) |
                        data[from + 2];
        data[to + 3] = base64[whole_triplet % 64];
        data[to + 2] = base64[(whole_triplet >> 6) % 64];
        data[to + 1] = base64[(whole_triplet >> 12) % 64];
        data[to] = base64[(whole_triplet >> 18) % 64];

        to -= 4;
        from -= 3;
        left_on_line--;
    }

    gw_assert(left_on_line == 0);
    gw_assert(from == -3);
    gw_assert(to == -4);

    /* Insert padding characters in the last quad.  Remember that
     * there is a CR LF between the last quad and the end of the
     * string. */
    switch (orig_len % 3) {
    case 0:
        break;
    case 1:
        gw_assert(data[ostr->len - 3] == 'A');
        gw_assert(data[ostr->len - 4] == 'A');
        data[ostr->len - 3] = '=';
        data[ostr->len - 4] = '=';
        break;
    case 2:
        gw_assert(data[ostr->len - 3] == 'A');
        data[ostr->len - 3] = '=';
        break;
    }

    seems_valid(ostr);
}


void octstr_base64_to_binary(Octstr *ostr)
{
    long triplet;
    long pos, len;
    long to;
    int quadpos = 0;
    int warned = 0;
    unsigned char *data;

    seems_valid(ostr);
    gw_assert(!ostr->immutable);

    len = ostr->len;
    data = ostr->data;

    if (len == 0)
        return;

    to = 0;
    triplet = 0;
    quadpos = 0;
    for (pos = 0; pos < len; pos++) {
        int c = data[pos];
        int sixbits;

        if (c >= 'A' && c <= 'Z') {
            sixbits = c - 'A';
        } else if (c >= 'a' && c <= 'z') {
            sixbits = 26 + c - 'a';
        } else if (c >= '0' && c <= '9') {
            sixbits = 52 + c - '0';
        } else if (c == '+') {
            sixbits = 62;
        } else if (c == '/') {
            sixbits = 63;
        } else if (c == '=') {
            /* These can only occur at the end of encoded
             * text.  RFC 2045 says we can assume it really
             * is the end. */
            break;
        } else if (isspace(c)) {
            /* skip whitespace */
            continue;
        } else {
            if (!warned) {
                warning(0, "Unusual characters in base64 "
                        "encoded text.");
                warned = 1;
            }
            continue;
        }

        triplet = (triplet << 6) | sixbits;
        quadpos++;

        if (quadpos == 4) {
            data[to++] = (triplet >> 16) & 0xff;
            data[to++] = (triplet >> 8) & 0xff;
            data[to++] = triplet & 0xff;
            quadpos = 0;
        }
    }

    /* Deal with leftover octets */
    switch (quadpos) {
    case 0:
        break;
    case 3:  /* triplet has 18 bits, we want the first 16 */
        data[to++] = (triplet >> 10) & 0xff;
        data[to++] = (triplet >> 2) & 0xff;
        break;
    case 2:  /* triplet has 12 bits, we want the first 8 */
        data[to++] = (triplet >> 4) & 0xff;
        break;
    case 1:
        warning(0, "Bad padding in base64 encoded text.");
        break;
    }

    ostr->len = to;
    data[to] = '\0';

    seems_valid(ostr);
}


long octstr_parse_long(long *nump, Octstr *ostr, long pos, int base)
{
    /* strtol wants a char *, and we have to compare the result to
     * an unsigned char *.  The easiest way to avoid warnings without
     * introducing typecasts is to use two variables. */
    char *endptr;
    unsigned char *endpos;
    long number;

    seems_valid(ostr);
    gw_assert(nump != NULL);
    gw_assert(base == 0 || (base >= 2 && base <= 36));

    if (pos >= ostr->len) {
        errno = EINVAL;
        return -1;
    }

    errno = 0;
    number = strtol(ostr->data + pos, &endptr, base);
    endpos = endptr;
    if (errno == ERANGE)
        return -1;
    if (endpos == ostr->data + pos) {
        errno = EINVAL;
        return -1;
    }

    *nump = number;
    return endpos - ostr->data;
}


int octstr_check_range(Octstr *ostr, long pos, long len,
                       octstr_func_t filter)
{
    long end = pos + len;

    seems_valid(ostr);
    gw_assert(len >= 0);

    if (pos >= ostr->len)
        return 1;
    if (end > ostr->len)
        end = ostr->len;

    for ( ; pos < end; pos++) {
        if (!filter(ostr->data[pos]))
            return 0;
    }

    return 1;
}


void octstr_convert_range(Octstr *ostr, long pos, long len,
                          octstr_func_t map)
{
    long end = pos + len;

    seems_valid(ostr);
    gw_assert(!ostr->immutable);
    gw_assert(len >= 0);

    if (pos >= ostr->len)
        return;
    if (end > ostr->len)
        end = ostr->len;

    for ( ; pos < end; pos++) {
        ostr->data[pos] = map(ostr->data[pos]);
    }

    seems_valid(ostr);
}


int octstr_compare(Octstr *ostr1, Octstr *ostr2)
{
    int ret;
    long len;

    seems_valid(ostr1);
    seems_valid(ostr2);

    if (ostr1->len < ostr2->len)
        len = ostr1->len;
    else
        len = ostr2->len;

    if (len == 0) {
	if (ostr1->len == 0 && ostr2->len > 0)
	    return -1;
	if (ostr1->len > 0 && ostr2->len == 0)
	    return 1;
        return 0;
    }

    ret = memcmp(ostr1->data, ostr2->data, len);
    if (ret == 0) {
        if (ostr1->len < ostr2->len)
            ret = -1;
        else if (ostr1->len > ostr2->len)
            ret = 1;
    }
    return ret;
}


int octstr_case_compare(Octstr *os1, Octstr *os2)
{
    int c1, c2;
    long i, len;

    seems_valid(os1);
    seems_valid(os2);

    if (os1->len < os2->len)
        len = os1->len;
    else
        len = os2->len;

    if (len == 0) {
	if (os1->len == 0 && os2->len > 0)
	    return -1;
	if (os1->len > 0 && os2->len == 0)
	    return 1;
        return 0;
    }

    for (i = 0; i < len; ++i) {
	c1 = toupper(os1->data[i]);
	c2 = toupper(os2->data[i]);
	if (c1 != c2)
	    break;
    }

    if (i == len) {
	if (i == os1->len && i == os2->len)
	    return 0;
	if (i == os1->len)
	    return -1;
	return 1;
    } else {
	c1 = toupper(os1->data[i]);
	c2 = toupper(os2->data[i]);
	if (c1 < c2)
	    return -1;
	if (c1 == c2)
	    return 0;
	return 1;
    }
}


int octstr_ncompare(Octstr *ostr1, Octstr *ostr2, long n)
{
    long len;

    seems_valid(ostr1);
    seems_valid(ostr2);

    if ((ostr1->len < ostr2->len) && (ostr1->len < n))
        len = ostr1->len;
    else if ((ostr2->len < ostr1->len) && (ostr2->len < n))
        len = ostr2->len;
    else
        len = n;

    if (len == 0)
        return 0;

    return memcmp(ostr1->data, ostr2->data, len);
}


int octstr_str_compare(Octstr *ostr, const char *str)
{
    seems_valid(ostr);

    if (str == NULL)
        return -1;
    if (ostr->data == NULL)
	return strcmp("", str);

    return strcmp(ostr->data, str);
}


int octstr_search_char(Octstr *ostr, int ch, long pos)
{
    unsigned char *p;

    seems_valid(ostr);
    gw_assert(ch >= 0);
    gw_assert(ch <= UCHAR_MAX);
    gw_assert(pos >= 0);

    if (pos >= ostr->len)
        return -1;

    p = memchr(ostr->data + pos, ch, ostr->len - pos);
    if (!p)
        return -1;
    return p - ostr->data;
}


int octstr_search(Octstr *haystack, Octstr *needle, long pos)
{
    int first;

    seems_valid(haystack);
    seems_valid(needle);
    gw_assert(pos >= 0);

    /* Always "find" an empty string */
    if (needle->len == 0)
        return 0;

    if (needle->len == 1)
        return octstr_search_char(haystack, needle->data[0], pos);

    /* For each occurrence of needle's first character in ostr,
     * check if the rest of needle follows.  Stop if there are no
     * more occurrences, or if the rest of needle can't possibly
     * fit in the haystack. */
    first = needle->data[0];
    pos = octstr_search_char(haystack, first, pos);
    while (pos >= 0 && haystack->len - pos >= needle->len) {
        if (memcmp(haystack->data + pos,
                   needle->data, needle->len) == 0)
            return pos;
        pos = octstr_search_char(haystack, first, pos + 1);
    }

    return -1;
}


int octstr_case_search(Octstr *haystack, Octstr *needle, long pos)
{
    long i, j;
    int c1, c2;

    seems_valid(haystack);
    seems_valid(needle);
    gw_assert(pos >= 0);

    /* Always "find" an empty string */
    if (needle->len == 0)
        return 0;

    for (i = pos; i <= haystack->len - needle->len; ++i) {
	for (j = 0; j < needle->len; ++j) {
	    c1 = toupper(haystack->data[i + j]);
	    c2 = toupper(needle->data[j]);
	    if (c1 != c2)
	    	break;
	}
	if (j == needle->len)
	    return i;
    }

    return -1;    
}


int octstr_print(FILE *f, Octstr *ostr)
{
    gw_assert(f != NULL);
    seems_valid(ostr);

    if (ostr->len == 0)
        return 0;
    if (fwrite(ostr->data, ostr->len, 1, f) != 1) {
        error(errno, "Couldn't write all of octet string to file.");
        return -1;
    }
    return 0;
}


int octstr_pretty_print(FILE *f, Octstr *ostr)
{
    unsigned char *p;
    long i;

    gw_assert(f != NULL);
    seems_valid(ostr);

    p = ostr->data;
    for (i = 0; i < ostr->len; ++i, ++p) {
        if (isprint(*p))
            fprintf(f, "%c", *p);
        else
            fprintf(f, "\\x%02x", *p);
    }
    if (ferror(f))
        return -1;
    return 0;
}


int octstr_write_to_socket(int socket, Octstr *ostr)
{
    long len;
    unsigned char *data;
    int ret;

    gw_assert(socket >= 0);
    seems_valid(ostr);

    data = ostr->data;
    len = ostr->len;
    while (len > 0) {
        ret = write(socket, data, len);
        if (ret == -1) {
            if (errno != EINTR) {
                error(errno, "Writing to socket failed");
                return -1;
            }
        } else {
            /* ret may be less than len */
            len -= ret;
            data += ret;
        }
    }
    return 0;
}


long octstr_write_data(Octstr *ostr, int fd, long from)
{
    long ret;

    gw_assert(fd >= 0);
    gw_assert(from >= 0);
    seems_valid(ostr);

    if (from >= ostr->len)
        return 0;

    ret = write(fd, ostr->data + from, ostr->len - from);

    if (ret < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        error(errno, "Error writing %ld octets to fd %d:",
              ostr->len - from, fd);
        return -1;
    }

    return ret;
}


int octstr_append_from_socket(Octstr *ostr, int socket)
{
    unsigned char buf[4096];
    int len;

    seems_valid(ostr);
    gw_assert(!ostr->immutable);

again:
    len = recv(socket, buf, sizeof(buf), 0);
    if (len < 0 && errno == EINTR)
        goto again;

    if (len < 0) {
        error(errno, "Could not read from socket %d", socket);
        return -1;
    }

    octstr_append_data(ostr, buf, len);
    return len;
}


void octstr_insert(Octstr *ostr1, Octstr *ostr2, long pos)
{
    seems_valid(ostr1);
    seems_valid(ostr2);
    gw_assert(pos <= ostr1->len);
    gw_assert(!ostr1->immutable);

    if (ostr2->len == 0)
        return;

    octstr_grow(ostr1, ostr1->len + ostr2->len);
    memmove(ostr1->data + pos + ostr2->len, ostr1->data + pos,
            ostr1->len - pos);
    memcpy(ostr1->data + pos, ostr2->data, ostr2->len);
    ostr1->len += ostr2->len;
    ostr1->data[ostr1->len] = '\0';

    seems_valid(ostr1);
}


void octstr_truncate(Octstr *ostr, int new_len)
{
    seems_valid(ostr);
    gw_assert(!ostr->immutable);
    gw_assert(new_len >= 0);

    if (new_len >= ostr->len)
        return;

    ostr->len = new_len;
    ostr->data[new_len] = '\0';

    seems_valid(ostr);
}


void octstr_strip_blanks(Octstr *text)
{
    int start = 0, end, len = 0;

    seems_valid(text);
    gw_assert(!text->immutable);

    /* Remove white space from the beginning of the text */
    while (isspace(octstr_get_char(text, start)) && 
	   start <= octstr_len(text))
        start ++;

    if (start > 0)
        octstr_delete(text, 0, start);

    /* and from the end. */

    if ((len = octstr_len(text)) > 0) {
        end = len = len - 1;
        while (isspace(octstr_get_char(text, end)) && end >= 0)
            end--;
        octstr_delete(text, end + 1, len - end);
    }

    seems_valid(text);
}

static int iscrlf(unsigned char c)
{
    return c == '\n' || c == '\r';
}

void octstr_strip_crlfs(Octstr *text)
{
    int start = 0, end, len = 0;

    seems_valid(text);
    gw_assert(!text->immutable);

    /* Remove white space from the beginning of the text */
    while (iscrlf(octstr_get_char(text, start)) && 
	   start <= octstr_len(text))
        start ++;

    if (start > 0)
        octstr_delete(text, 0, start);

    /* and from the end. */

    if ((len = octstr_len(text)) > 0) {
        end = len = len - 1;
        while (iscrlf(octstr_get_char(text, end)) && end >= 0)
            end--;
        octstr_delete(text, end + 1, len - end);
    }

    seems_valid(text);
}

void octstr_strip_nonalphanums(Octstr *text)
{
    int start = 0, end, len = 0;

    seems_valid(text);
    gw_assert(!text->immutable);

    /* Remove white space from the beginning of the text */
    while (!isalnum(octstr_get_char(text, start)) && 
	   start <= octstr_len(text))
        start ++;

    if (start > 0)
        octstr_delete(text, 0, start);

    /* and from the end. */

    if ((len = octstr_len(text)) > 0) {
        end = len = len - 1;
        while (!isalnum(octstr_get_char(text, end)) && end >= 0)
            end--;
        octstr_delete(text, end + 1, len - end);
    }

    seems_valid(text);
}


void octstr_shrink_blanks(Octstr *text)
{
    int i, j, end;

    seems_valid(text);
    gw_assert(!text->immutable);

    end = octstr_len(text);

    /* Shrink white spaces to one  */
    for (i = 0; i < end; i++) {
        if (isspace(octstr_get_char(text, i))) {
            /* Change the remaining space into single space. */
            if (octstr_get_char(text, i) != ' ')
                octstr_set_char(text, i, ' ');

            j = i = i + 1;
            while (isspace(octstr_get_char(text, j)))
                j ++;
            if (j - i > 1)
                octstr_delete(text, i, j - i);
        }
    }

    seems_valid(text);
}


void octstr_insert_data(Octstr *ostr, long pos, const char *data, long len)
{
    seems_valid(ostr);
    gw_assert(!ostr->immutable);
    gw_assert(pos <= ostr->len);

    if (len == 0)
        return;

    octstr_grow(ostr, ostr->len + len);
    if (ostr->len > pos) {	/* only if neccessary*/
        memmove(ostr->data + pos + len, ostr->data + pos, ostr->len - pos);
    }
    memcpy(ostr->data + pos, data, len);
    ostr->len += len;
    ostr->data[ostr->len] = '\0';

    seems_valid(ostr);
}

void octstr_insert_char(Octstr *ostr, long pos, const char c)
{
    seems_valid(ostr);
    gw_assert(!ostr->immutable);
    gw_assert(pos <= ostr->len);
    
    octstr_grow(ostr, ostr->len + 1);
    if (ostr->len > pos)
        memmove(ostr->data + pos + 1, ostr->data + pos, ostr->len - pos);
    memcpy(ostr->data + pos, &c, 1);
    ostr->len += 1;
    ostr->data[ostr->len] = '\0';
    
    seems_valid(ostr);
}

void octstr_append_data(Octstr *ostr, const char *data, long len)
{
    gw_assert(ostr != NULL);
    octstr_insert_data(ostr, ostr->len, data, len);
}


void octstr_append(Octstr *ostr1, Octstr *ostr2)
{
    gw_assert(ostr1 != NULL);
    octstr_insert(ostr1, ostr2, ostr1->len);
}


void octstr_append_cstr(Octstr *ostr, const char *cstr)
{
    octstr_insert_data(ostr, ostr->len, cstr, strlen(cstr));
}


void octstr_append_char(Octstr *ostr, int ch)
{
    unsigned char c = ch;

    gw_assert(ch >= 0);
    gw_assert(ch <= UCHAR_MAX);
    octstr_insert_data(ostr, ostr->len, &c, 1);
}


void octstr_delete(Octstr *ostr1, long pos, long len)
{
    seems_valid(ostr1);
    gw_assert(!ostr1->immutable);

    if (pos > ostr1->len)
        pos = ostr1->len;
    if (pos + len > ostr1->len)
        len = ostr1->len - pos;
    if (len > 0) {
        memmove(ostr1->data + pos, ostr1->data + pos + len,
                ostr1->len - pos - len);
        ostr1->len -= len;
        ostr1->data[ostr1->len] = '\0';
    }

    seems_valid(ostr1);
}



Octstr *octstr_read_file(const char *filename)
{
    FILE *f;
    Octstr *os;
    char buf[4096];
    long n;

    gw_assert(filename != NULL);

    f = fopen(filename, "r");
    if (f == NULL) {
        error(errno, "fopen failed: couldn't open `%s'", filename);
        return NULL;
    }

    os = octstr_create("");
    if (os == NULL)
        goto error;

    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        octstr_insert_data(os, octstr_len(os), buf, n);

    (void) fclose(f);
    return os;

error:
    (void) fclose(f);
    octstr_destroy(os);
    return NULL;
}


Octstr *octstr_read_pipe(FILE *f)
{
    Octstr *os;
    char buf[4096];

    gw_assert(f != NULL);

    os = octstr_create("");
    if (os == NULL)
        goto error;

    while (fgets(buf, sizeof(buf), f) != NULL)
        octstr_append_data(os, buf, strlen(buf));

    return os;

error:
    octstr_destroy(os);
    return NULL;
}


List *octstr_split_words(Octstr *ostr)
{
    unsigned char *p;
    List *list;
    Octstr *word;
    long i, start, end;

    seems_valid(ostr);

    list = list_create();

    p = ostr->data;
    i = 0;
    for (; ; ) {
        while (i < ostr->len && isspace(*p)) {
            ++p;
            ++i;
        }
        start = i;

        while (i < ostr->len && !isspace(*p)) {
            ++p;
            ++i;
        }
        end = i;

        if (start == end)
            break;

        word = octstr_create_from_data(ostr->data + start,
                                       end - start);
        list_append(list, word);
    }

    return list;
}


List *octstr_split(Octstr *os, Octstr *sep)
{
    List *list;
    long next, pos, seplen;
    
    list = list_create();
    pos = 0;
    seplen = octstr_len(sep);

    while ((next = octstr_search(os, sep, pos)) != -1) {
	list_append(list, octstr_copy(os, pos, next - pos));
	pos = next + seplen;
    }
    
    if (pos < octstr_len(os))
    	list_append(list, octstr_copy(os, pos, octstr_len(os)));
    
    return list;
}


int octstr_item_match(void *item, void *pattern)
{
    return octstr_compare(item, pattern) == 0;
}


void octstr_dump(Octstr *ostr, int level)
{
    unsigned char *p, *d, buf[1024], charbuf[256];
    long pos;
    const int octets_per_line = 8;
    int c, this_line_begins_at;

    if (ostr == NULL)
        return;

    seems_valid(ostr);

    debug("gwlib.octstr", 0, "%*sOctet string at %p:", level, "",
          (void *) ostr);
    debug("gwlib.octstr", 0, "%*s  len:  %lu", level, "",
          (unsigned long) ostr->len);
    debug("gwlib.octstr", 0, "%*s  size: %lu", level, "",
          (unsigned long) ostr->size);
    debug("gwlib.octstr", 0, "%*s  immutable: %d", level, "",
          ostr->immutable);

    buf[0] = '\0';
    p = buf;
    d = charbuf;
    this_line_begins_at = 0;
    for (pos = 0; pos < octstr_len(ostr); ) {
        c = octstr_get_char(ostr, pos);
        sprintf(p, "%02x ", c);
        p = strchr(p, '\0');
        if (isprint(c))
            *d++ = c;
        else
            *d++ = '.';
        ++pos;
        if (pos - this_line_begins_at == octets_per_line) {
            *d = '\0';
            debug("gwlib.octstr", 0, "%*s  data: %s  %s", level, "",
                  buf, charbuf);
            buf[0] = '\0';
            charbuf[0] = '\0';
            p = buf;
            d = charbuf;
            this_line_begins_at = pos;
        }
    }
    if (pos - this_line_begins_at > 0) {
        *d = '\0';
        debug("gwlib.octstr", 0, "%*s  data: %-*.*s  %s", level, "",
              octets_per_line*3,
              octets_per_line*3, buf, charbuf);
    }

    debug("gwlib.octstr", 0, "%*sOctet string dump ends.", level, "");
}

void octstr_dump_short(Octstr *ostr, int level, const char *name)
{
    char buf[100];
    char *p;
    long i;
    int c;

    if (ostr == NULL) {
        debug("gwlib.octstr", 0, "%*s%s: NULL", level, "", name);
        return;
    }

    seems_valid(ostr);

    if (ostr->len < 20) {
        p = buf;
        for (i = 0; i < ostr->len; i++) {
            c = ostr->data[i];
            if (c == '\n') {
                *p++ = '\\';
                *p++ = 'n';
            } else if (!isprint(c)) {
                break;
            } else if (c == '"') {
                *p++ = '\\';
                *p++ = '"';
            } else if (c == '\\') {
                *p++ = '\\';
                *p++ = '\\';
            } else {
                *p++ = c;
            }
        }
        if (i == ostr->len) {
            *p++ = 0;
            /* We got through the loop without hitting nonprintable
             * characters. */
            debug("gwlib.octstr", 0, "%*s%s: \"%s\"", level, "", name, buf);
            return;
        }
    }

    debug("gwlib.octstr", 0, "%*s%s:", level, "", name);
    octstr_dump(ostr, level + 1);
}

void octstr_url_encode(Octstr *ostr)
{
    int i, n, newlen;
    unsigned char *str, *str2, *hexits;
    unsigned char c;
    Octstr *res;

    seems_valid(ostr);

    if (ostr->len == 0)
	return;

    res = octstr_create("");
    str = ostr->data;
    n = 0;
    for (i = 0; i < ostr->len; i++)
	if (!is_safe[*str++])
	    n++;
    newlen = ostr->len + 2 * n;
    res->len = newlen;
    res->size = res->len + 1;
    res->data = gw_malloc(res->size);

    hexits = "0123456789ABCDEF";

    str = ostr->data;
    str2 = res->data;
    for (i = 0; i < ostr->len; i++) {
	c = *str++;
	if (!is_safe[c]) {
	    *str2++ = '%';
	    *str2++ = hexits[c >> 4 & 0xf];
	    *str2++ = hexits[c & 0xf];
	}
	else if (c == ' ')
	    *str2++ = '+';
	else
	    *str2++ = c;
    }
    *str2 = 0;
    seems_valid(res);
    
    octstr_truncate(ostr, 0);
    octstr_insert(ostr, res, 0);
    octstr_destroy(res);
}


int octstr_url_decode(Octstr *ostr)
{
    long value;
    unsigned char *string = ostr->data;
    unsigned char *dptr = ostr->data;
    unsigned char buf[3];    	/* buffer for strtol conversion */
    buf[2] = '\0';

    seems_valid(ostr);
    gw_assert(!ostr->immutable);

    if (ostr->len == 0)
        return 0;

    do {
        if (*string == '%') {
            if (*(string + 1) == '\0' || *(string + 2) == '\0')
                goto error;
            buf[0] = *(string + 1);
            buf[1] = *(string + 2);
            value = strtol(buf, NULL, 16);

            if (value >= 0 && value < 256) {
                *dptr = (unsigned char)value;
                string += 3;
                dptr++;
                continue;
            }
            warning(0, "Garbage encoded (value = %ld)", value);
        }
        if (*string == '+') {
            *dptr++ = ' ';
            string++;
        } else
            *dptr++ = *string++;
    } while (*string); 	/* we stop here because it terimates encoded string */
    *dptr = '\0';

    ostr->len = (dptr - ostr->data);

    seems_valid(ostr);
    return 0;

error:
    *dptr = '\0';
    ostr->len = (dptr - ostr->data);
    warning(0, "octstr_url_decode: corrupted end-of-string <%s>", string);
    seems_valid(ostr);
    return -1;
}


long octstr_get_bits(Octstr *ostr, long bitpos, int numbits)
{
    long pos;
    long result;
    int mask;
    int shiftwidth;

    seems_valid(ostr);
    gw_assert(bitpos >= 0);
    gw_assert(numbits <= 32);
    gw_assert(numbits >= 0);

    pos = bitpos / 8;
    bitpos = bitpos % 8;

    /* This also takes care of the len == 0 case */
    if (pos >= ostr->len)
        return 0;

    mask = (1 << numbits) - 1;

    /* It's easy if the range fits in one octet */
    if (bitpos + numbits <= 8) {
        /* shiftwidth is the number of bits to ignore on the right.
         * bitpos 0 is the leftmost bit. */
        shiftwidth = 8 - (bitpos + numbits);
        return (ostr->data[pos] >> shiftwidth) & mask;
    }

    /* Otherwise... */
    result = 0;
    while (bitpos + numbits > 8) {
        result = (result << 8) | ostr->data[pos];
        numbits -= (8 - bitpos);
        bitpos = 0;
        pos++;
        if (pos >= ostr->len)
            return (result << numbits) & mask;
    }

    gw_assert(bitpos == 0);
    result <<= numbits;
    result |= ostr->data[pos] >> (8 - numbits);
    return result & mask;
}

void octstr_set_bits(Octstr *ostr, long bitpos, int numbits,
                     unsigned long value)
{
    long pos;
    unsigned long mask;
    int shiftwidth;
    int bits;
    int maxlen;
    int c;

    seems_valid(ostr);
    gw_assert(!ostr->immutable);
    gw_assert(bitpos >= 0);
    gw_assert(numbits <= 32);
    gw_assert(numbits >= 0);

    maxlen = (bitpos + numbits + 7) / 8;
    if (maxlen > ostr->len) {
        octstr_grow(ostr, maxlen);
        /* Make sure the new octets start out with value 0 */
        for (pos = ostr->len; pos < maxlen; pos++) {
            ostr->data[pos] = 0;
        }
        ostr->len = maxlen;
        ostr->data[maxlen] = 0;
    }

    mask = (1 << numbits) - 1;
    /* mask is also the largest value that fits */
    gw_assert(value <= mask);

    pos = bitpos / 8;
    bitpos = bitpos % 8;

    /* Does the range fit in one octet? */
    if (bitpos + numbits <= 8) {
        /* shiftwidth is the number of bits to ignore on the right.
         * bitpos 0 is the leftmost bit. */
        shiftwidth = 8 - (bitpos + numbits);
        /* Extract the bits we don't want to affect */
        c = ostr->data[pos] & ~(mask << shiftwidth);
        c |= value << shiftwidth;
        gw_assert(pos < ostr->len);
        ostr->data[pos] = c;
        return;
    }

    /* Otherwise... */
    /* If speed is a problem here, we could have separate cases for
     * the first octet (which may have bitpos > 0), and the rest,
     * which don't. */
    while (bitpos + numbits > 8) {
        /* We want this many bits from the value */
        bits = 8 - bitpos;
        /* There are this many bits to their right in the value */
        shiftwidth = numbits - bits;
        /* Construct a mask for "bits" bits on the far right */
        mask = (1 << bits) - 1;
        /* Get the bits we want */
        c = (value >> shiftwidth) & mask;
        /* Merge them with the bits that are already there */
        gw_assert(pos < ostr->len);
        ostr->data[pos] = (ostr->data[pos] & ~mask) | c;
        numbits -= (8 - bitpos);
        bitpos = 0;
        pos++;
    }

    gw_assert(bitpos == 0);
    gw_assert(pos < ostr->len);
    /* Set remaining bits.  This is just like the single-octet case
     * before the loop, except that we know bitpos is 0. */
    mask = (1 << numbits) - 1;
    shiftwidth = 8 - numbits;
    c = ostr->data[pos] & ~(mask << shiftwidth);
    c |= value << shiftwidth;
    ostr->data[pos] = c;

    seems_valid(ostr);
}


void octstr_append_uintvar(Octstr *ostr, unsigned long value)
{
    /* A uintvar is defined to be up to 32 bits large, so it will
     * fit in 5 octets. */
    unsigned char octets[5];
    int i;
    int start;

    /* Handle last byte separately; it has no continuation bit,
     * and must be encoded even if value is 0. */
    octets[4] = value & 0x7f;
    value >>= 7;

    for (i = 3; value > 0 && i >= 0; i--) {
        octets[i] = 0x80 | (value & 0x7f);
        value >>= 7;
    }
    start = i + 1;

    octstr_append_data(ostr, octets + start, 5 - start);
}


long octstr_extract_uintvar(Octstr *ostr, unsigned long *value, long pos)
{
    int c;
    int count;
    unsigned long ui;

    ui = 0;
    for (count = 0; count < 5; count++) {
        c = octstr_get_char(ostr, pos + count);
        if (c < 0)
            return -1;
        ui = (ui << 7) | (c & 0x7f);
        if (!(c & 0x80)) {
            *value = ui;
            return pos + count + 1;
        }
    }

    return -1;
}


void octstr_append_decimal(Octstr *ostr, long value)
{
    char tmp[128];
    Octstr *ostmp;

    sprintf(tmp, "%ld", value);
    ostmp = octstr_create(tmp);
    octstr_append(ostr, ostmp);
    octstr_destroy(ostmp);
}


/**********************************************************************
 * octstr_format and related private functions
 */


/*
 * A parsed form of the format string. This struct has been carefully
 * defined so that it can be initialized with {0} and it will have 
 * the correct defaults.
 */
struct format
{
    int minus;
    int zero;

    long min_width;

    int has_prec;
    long prec;

    long type;
};


static void format_flags(struct format *format, const char **fmt)
{
    int done;

    done = 0;
    do
    {
        switch (**fmt) {
        case '-':
            format->minus = 1;
            break;

        case '0':
            format->zero = 1;
            break;

        default:
            done = 1;
        }

        if (!done)
            ++(*fmt);
    } while (!done);
}


static void format_width(struct format *format, const char **fmt,
                         VALPARM(args))
{
    char *end;

    if (**fmt == '*')
    {
        format->min_width = va_arg(VALST(args), int);
        ++(*fmt);
    } else if (isdigit(**(const unsigned char **) fmt))
    {
        format->min_width = strtol(*fmt, &end, 10);
        *fmt = end;
        /* XXX error checking is missing from here */
    }
}


static void format_prec(struct format *format, const char **fmt,
                        VALPARM(args))
{
    char *end;

    if (**fmt != '.')
        return;
    ++(*fmt);
    if (**fmt == '*')
    {
        format->has_prec = 1;
        format->prec = va_arg(VALST(args), int);
        ++(*fmt);
    } else if (isdigit(**(const unsigned char **) fmt))
    {
        format->has_prec = 1;
        format->prec = strtol(*fmt, &end, 10);
        *fmt = end;
        /* XXX error checking is missing from here */
    }
}


static void format_type(struct format *format, const char **fmt)
{
    switch (**fmt)
    {
    case 'h':
    case 'l':
        format->type = **fmt;
        ++(*fmt);
        break;
    }
}


static void convert(Octstr *os, struct format *format, const char **fmt,
                    VALPARM(args))
{
    Octstr *new;
    char *s, *pad;
    long n;
    unsigned long u;
    char tmpfmt[1024];
    char tmpbuf[1024];
    char c;
    void *p;

    new = NULL;

    switch (**fmt)
    {
    case 'c':
        c = va_arg(VALST(args), int);
        new = octstr_create_from_data(&c, 1);
        break;

    case 'd':
    case 'i':
        switch (format->type) {
        case 'l':
            n = va_arg(VALST(args), long);
            break;
        case 'h':
            n = (short) va_arg(VALST(args), int);
            break;
        default:
            n = va_arg(VALST(args), int);
            break;
        }
        new = octstr_create("");
        octstr_append_decimal(new, n);
        break;

    case 'o':
    case 'u':
    case 'x':
    case 'X':
	switch (format->type) {
	case 'l':
	    u = va_arg(VALST(args), unsigned long);
	    break;
        case 'h':
            u = (unsigned short) va_arg(VALST(args), unsigned int);
            break;
        default:
            u = va_arg(VALST(args), unsigned int);
            break;
        }
        tmpfmt[0] = '%';
	tmpfmt[1] = 'l';
	tmpfmt[2] = **fmt;
	tmpfmt[3] = '\0';
	sprintf(tmpbuf, tmpfmt, u);
        new = octstr_create(tmpbuf);
        break;

    case 'e':
    case 'f':
    case 'g':
        sprintf(tmpfmt, "%%");
        if (format->minus)
            strcat(tmpfmt, "-");
        if (format->zero)
            strcat(tmpfmt, "0");
        if (format->min_width > 0)
            sprintf(strchr(tmpfmt, '\0'),
                    "%ld", format->min_width);
        if (format->has_prec)
            sprintf(strchr(tmpfmt, '\0'),
                    ".%ld", format->prec);
        if (format->type != '\0')
            sprintf(strchr(tmpfmt, '\0'),
                    "%c", (int) format->type);
        sprintf(strchr(tmpfmt, '\0'), "%c", **fmt);
        snprintf(tmpbuf, sizeof(tmpbuf),
                 tmpfmt, va_arg(VALST(args), double));
        new = octstr_create(tmpbuf);
        break;

    case 's':
        s = va_arg(VALST(args), char *);
        if (format->has_prec && format->prec < (long) strlen(s))
            n = format->prec;
        else
            n = (long) strlen(s);
        new = octstr_create_from_data(s, n);
        break;

    case 'p':
    	p = va_arg(VALST(args), void *);
	sprintf(tmpfmt, "%p", p);
	new = octstr_create(tmpfmt);
	break;

    case 'S':
        new = octstr_duplicate(va_arg(VALST(args), Octstr *));
        if (!new)
            new = octstr_imm("(null)");
        if (format->has_prec)
            octstr_truncate(new, format->prec);
        break;

    case 'E':
        new = octstr_duplicate(va_arg(VALST(args), Octstr *));
        if (!new)
            new = octstr_imm("(null)");
	octstr_url_encode(new);
	/*
	 * note: we use blind truncate - encoded character can get cut half-way.
         */
        if (format->has_prec)
            octstr_truncate(new, format->prec);
        break;

    case '%':
    	new = octstr_imm("%");
    	break;

    default:
        panic(0, "octstr_format format string syntax error.");
    }

    if (format->zero)
        pad = "0";
    else
        pad = " ";

    if (format->minus) {
        while (format->min_width > octstr_len(new))
            octstr_append_data(new, pad, 1);
    } else {
        while (format->min_width > octstr_len(new))
            octstr_insert_data(new, 0, pad, 1);
    }

    octstr_append(os, new);
    octstr_destroy(new);

    if (**fmt != '\0')
        ++(*fmt);
}


Octstr *octstr_format(const char *fmt, ...)
{
    Octstr *os;
    va_list args;

    va_start(args, fmt);
    os = octstr_format_valist(fmt, args);
    va_end(args);
    return os;
}


Octstr *octstr_format_valist_real(const char *fmt, va_list args)
{
    Octstr *os;
    size_t n;

    os = octstr_create("");

    while (*fmt != '\0') {
        struct format format = { 0, };

        n = strcspn(fmt, "%");
        octstr_append_data(os, fmt, n);
        fmt += n;

        gw_assert(*fmt == '%' || *fmt == '\0');
        if (*fmt == '\0')
            continue;

        ++fmt;
        format_flags(&format, &fmt);
        format_width(&format, &fmt, VARGS(args));
        format_prec(&format, &fmt, VARGS(args));
        format_type(&format, &fmt);
        convert(os, &format, &fmt, VARGS(args));
    }

    seems_valid(os);
    return os;
}


void octstr_format_append(Octstr *os, const char *fmt, ...)
{
    Octstr *temp;
    va_list args;

    va_start(args, fmt);
    temp = octstr_format_valist(fmt, args);
    va_end(args);
    octstr_append(os, temp);
    octstr_destroy(temp);
}


unsigned long octstr_hash_key(Octstr *ostr)
{
    unsigned long key = 0;
    long i;

    if (ostr == NULL)
	return 0;

    for (i = 0; i < octstr_len(ostr); i++)
	key = key + octstr_get_char(ostr, i);

    return key;
}


/**********************************************************************
 * Local functions.
 */

static void seems_valid_real(Octstr *ostr, const char *filename, long lineno,
                             const char *function)
{
    gw_assert(immutables_init);
    gw_assert_place(ostr != NULL,
                    filename, lineno, function);
    gw_assert_allocated(ostr,
                        filename, lineno, function);
    gw_assert_place(ostr->len >= 0,
                    filename, lineno, function);
    gw_assert_place(ostr->size >= 0,
                    filename, lineno, function);
    if (ostr->size == 0) {
        gw_assert_place(ostr->len == 0,
                        filename, lineno, function);
        gw_assert_place(ostr->data == NULL,
                        filename, lineno, function);
    } else {
        gw_assert_place(ostr->len + 1 <= ostr->size,
                        filename, lineno, function);
        gw_assert_place(ostr->data != NULL,
                        filename, lineno, function);
	if (!ostr->immutable)
            gw_assert_allocated(ostr->data,
                                filename, lineno, function);
        gw_assert_place(ostr->data[ostr->len] == '\0',
                        filename, lineno, function);
    }
}

int
octstr_recode (Octstr *tocode, Octstr *fromcode, Octstr *orig)
{
    Octstr *octstr_utf8 = NULL;
    Octstr *octstr_final = NULL;
    int resultcode = 0;
    
    if (octstr_case_compare(tocode, fromcode) == 0) {
	goto cleanup_and_exit;
    }

    if ((octstr_case_compare(fromcode, octstr_imm ("utf-8")) != 0) &&
	(octstr_case_compare(fromcode, octstr_imm ("utf8")) != 0)) {
	if (charset_to_utf8(orig, &octstr_utf8, fromcode) < 0) {
	    resultcode = -1;
	    goto cleanup_and_exit;
	}
    } else {
	octstr_utf8 = octstr_duplicate(orig);
    }

    if ((octstr_case_compare(tocode, octstr_imm ("utf-8")) != 0) &&
	(octstr_case_compare(tocode, octstr_imm ("utf8")) != 0)) {
	if (charset_from_utf8(octstr_utf8, &octstr_final, tocode) < 0) {
	    resultcode = -1;
	    goto cleanup_and_exit;
	}
    } else {
	octstr_final = octstr_duplicate(octstr_utf8);
    }

    octstr_truncate(orig, 0);
    octstr_append(orig, octstr_final);

 cleanup_and_exit:
    octstr_destroy (octstr_utf8);
    octstr_destroy (octstr_final);

    return resultcode;
}

void octstr_strip_char(Octstr *text, char ch)
{
    int start = 0;

    seems_valid(text);
    gw_assert(!text->immutable);

    /* Remove char from the beginning of the text */
    while ((ch == octstr_get_char(text, start)) &&
           start <= octstr_len(text))
        start ++;

    if (start > 0)
        octstr_delete(text, 0, start);

    seems_valid(text);
}

int octstr_isnum(Octstr *ostr1)
{
    int start = 0;
    char c;

    seems_valid(ostr1);
    while (start < octstr_len(ostr1)) {
        c = octstr_get_char(ostr1, start);
        if (!isdigit(c) && (c!='+'))
            return 0;
        start++;
    }
    return 1;
}

void octstr_replace(Octstr *haystack, Octstr *needle, Octstr *repl)
{
    int p = -1;
    long len;

    len = octstr_len(needle);

    while ((p = octstr_search(haystack, needle, p + 1)) != -1) {
        octstr_delete(haystack, p, len);
        octstr_insert(haystack, repl, p);
    }
}

int octstr_symbolize(Octstr *ostr)
{
    long len, i;

    seems_valid(ostr);
    gw_assert(!ostr->immutable);

    if (ostr->len == 0)
        return 0;

    /* Check if it's in the right format */
    if (!octstr_check_range(ostr, 0, ostr->len, gw_isxdigit))
        return -1;

    len = ostr->len + (ostr->len/2);
    octstr_grow(ostr, ostr->len * 2);

    for (i = 0; i < len; i += 3)
        octstr_insert_data(ostr, i, "%", 1);

    return 1;
}
