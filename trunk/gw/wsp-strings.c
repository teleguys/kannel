/* wsp-strings.c: lookup code for various tables defined by WSP standard
 *
 * This file provides functions to translate strings to numbers and numbers
 * to strings according to the Assigned Numbers tables in appendix A
 * of the WSP specification.
 *
 * The tables are in wsp-strings.def, in a special format suitable for
 * use with the C preprocessor, which we abuse liberally to get the
 * interface we want. 
 *
 * Richard Braakman
 */

#include "gwlib/gwlib.h"
#include "wsp-strings.h"

#define TABLE_SIZE(table) ((long)(sizeof(table) / sizeof(table[0])))

static int initialized;

/* The arrays in a table structure are all of equal length, and their
 * elements correspond.  The number for string 0 is in numbers[0], etc.
 * Table structures are initialized dynamically.
 */
struct table
{
    long size;          /* Nr of entries in each of the tables below */
    Octstr **strings;   /* Immutable octstrs */
    long *numbers;      /* Assigned numbers, or NULL for linear tables */
    int linear;	    /* True for tables defined as LINEAR */
};

struct element
{
    unsigned char *str;
    long number;
};

/* Local functions */
static Octstr *number_to_string(long number, struct table *table);
static unsigned char *number_to_cstr(long number, struct table *table);
static long string_to_number(Octstr *ostr, struct table *table);


/* Declare the data.  For each table "foo", create a foo_strings array
 * to hold the data, and a (still empty) foo_table structure. */
#define LINEAR(name, strings) \
    static const unsigned char *name##_strings[] = { strings }; \
    static struct table name##_table;
#define STRING(string) string,
#define NUMBERED(name, strings) \
    static const struct element name##_strings[] = { strings }; \
    static struct table name##_table;
#define ASSIGN(string, number) { string, number },
#include "wsp-strings.def"

/* Define the functions for translating number to Octstr */
#define LINEAR(name, strings) \
Octstr *wsp_##name##_to_string(long number) { \
    return number_to_string(number, &name##_table); \
}
#include "wsp-strings.def"

/* Define the functions for translating number to constant string */
#define LINEAR(name, strings) \
unsigned char *wsp_##name##_to_cstr(long number) { \
    return number_to_cstr(number, &name##_table); \
}
#include "wsp-strings.def"

#define LINEAR(name, strings) \
long wsp_string_to_##name(Octstr *ostr) { \
     return string_to_number(ostr, &name##_table); \
}
#include "wsp-strings.def"

static Octstr *number_to_string(long number, struct table *table)
{
    long i;

    gw_assert(initialized);

    if (table->linear) {
	if (number >= 0 && number < table->size)
	    return octstr_duplicate(table->strings[number]);
    } else {
	for (i = 0; i < table->size; i++) {
   	     if (table->numbers[i] == number)
		return octstr_duplicate(table->strings[i]);
	}
    }
    return NULL;
}

static unsigned char *number_to_cstr(long number, struct table *table)
{
    long i;

    gw_assert(initialized);

    if (table->linear) {
	if (number >= 0 && number < table->size)
	    return octstr_get_cstr(table->strings[number]);
    } else {
	for (i = 0; i < table->size; i++) {
   	     if (table->numbers[i] == number)
		return octstr_get_cstr(table->strings[i]);
	}
    }
    return NULL;
}

/* Case-insensitive string lookup */
static long string_to_number(Octstr *ostr, struct table *table)
{
    long i;

    gw_assert(initialized);

    for (i = 0; i < table->size; i++) {
	if (octstr_case_compare(ostr, table->strings[i]) == 0) {
	    return table->linear ? i : table->numbers[i];
	}
    }

    return -1;
}

static void construct_linear_table(struct table *table,
const unsigned char **strings, long size)
{
    long i;

    table->size = size;
    table->strings = gw_malloc(size * (sizeof table->strings[0]));
    table->numbers = NULL;
    table->linear = 1;

    for (i = 0; i < size; i++) {
	table->strings[i] = octstr_create_immutable(strings[i]);
    }
}

static void construct_numbered_table(struct table *table,
const struct element *strings, long size)
{
    long i;

    table->size = size;
    table->strings = gw_malloc(size * (sizeof table->strings[0]));
    table->numbers = gw_malloc(size * (sizeof table->numbers[0]));
    table->linear = 0;

    for (i = 0; i < size; i++) {
	table->strings[i] = octstr_create_immutable(strings[i].str);
	table->numbers[i] = strings[i].number;
    }
}

static void destroy_table(struct table *table)
{
    /* No need to call octstr_destroy on immutable octstrs */

    gw_free(table->strings);
    gw_free(table->numbers);
}

void wsp_strings_init(void)
{
#define LINEAR(name, strings) \
    construct_linear_table(&name##_table, \
		name##_strings, TABLE_SIZE(name##_strings));
#define NUMBERED(name, strings) \
    construct_numbered_table(&name##_table, \
	        name##_strings, TABLE_SIZE(name##_strings));
#include "wsp-strings.def"
    initialized = 1;
}

void wsp_strings_shutdown(void)
{
    initialized = 0;
#define LINEAR(name, strings) \
    destroy_table(&name##_table);
#include "wsp-strings.def"
}
