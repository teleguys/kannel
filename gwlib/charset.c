/*
 * gwlib/charset.c - character set conversions
 *
 * This file implements the character set conversions declared in charset.h.
 *
 * Richard Braakman
 */

#include "gwlib/gwlib.h"

/* Map GSM default alphabet characters to ISO-Latin-1 characters.
 * The greek characters at positions 16 and 18 through 26 are not
 * mappable.  They are mapped to '?' characters.
 * The escape character, at position 27, is mapped to a space,
 * though normally the function that indexes into this table will
 * treat it specially. */
const static unsigned char gsm_to_latin1[128] = {
	 '@', 0xa3,  '$', 0xa5, 0xe8, 0xe9, 0xf9, 0xec,   /* 0 - 7 */
	0xf2, 0xc7,   10, 0xd8, 0xf8,   13, 0xc5, 0xe5,   /* 8 - 15 */
	 '?',  '_',  '?',  '?',  '?',  '?',  '?',  '?',   /* 16 - 23 */
         '?',  '?',  '?',  ' ', 0xc6, 0xe6, 0xdf, 0xc9,   /* 24 - 31 */
	 ' ',  '!',  '"',  '#', 0xa4,  '%',  '&', '\'',   /* 32 - 39 */
	 '(',  ')',  '*',  '+',  ',',  '-',  '.',  '/',   /* 40 - 47 */
	 '0',  '1',  '2',  '3',  '4',  '5',  '6',  '7',   /* 48 - 55 */
	 '8',  '9',  ':',  ';',  '<',  '=',  '>',  '?',   /* 56 - 63 */
        0xa1,  'A',  'B',  'C',  'D',  'E',  'F',  'G',   /* 64 - 71 */
         'H',  'I',  'J',  'K',  'L',  'M',  'N',  'O',   /* 73 - 79 */
         'P',  'Q',  'R',  'S',  'T',  'U',  'V',  'W',   /* 80 - 87 */
         'X',  'Y',  'Z', 0xc4, 0xd6, 0xd1, 0xdc, 0xa7,   /* 88 - 95 */
        0xbf,  'a',  'b',  'c',  'd',  'e',  'f',  'g',   /* 96 - 103 */
         'h',  'i',  'j',  'k',  'l',  'm',  'n',  'o',   /* 104 - 111 */
         'p',  'q',  'r',  's',  't',  'u',  'v',  'w',   /* 112 - 119 */
         'x',  'y',  'z', 0xe4, 0xf6, 0xf1, 0xfc, 0xe0    /* 120 - 127 */
};

/* This is the extension table defined in GSM 03.38.  It is the mapping
 * used for the character after a GSM 27 (Escape) character.  All characters
 * not in the table, as well as characters we can't represent, will map
 * to themselves.  We cannot represent the euro symbol, which is an escaped
 * 'e', so we left it out of this table. */
const static struct {
	int gsmesc;
	int latin1;
} gsm_escapes[] = {
	{ 10, 12 }, /* ASCII page break */
	{ 20, '^' },
	{ 40, '{' },
	{ 41, '}' },
	{ 47, '\\' },
	{ 60, '[' },
	{ 61, '~' },
	{ 62, ']' },
	{ 64, '|' },
	{ -1, -1 }
};

/* Code used for non-representable characters */
#define NRP '?'

/* Map ISO-Latin-1 characters to the GSM default alphabet.  Negative
 * encoded as ESC (code 27) followed by the absolute value of the
 * number. */
const static int latin1_to_gsm[256] = {
	NRP, NRP, NRP, NRP, NRP, NRP, NRP, NRP,       /* 0 - 7 */
	/* TAB approximates to space */
	/* LF and CR map to self */
	/* Page break maps to escaped LF */
	NRP, ' ',  10, NRP, -10,  13, NRP, NRP,       /* 8 - 15 */
	/* 16, 18-26 are nonprintable in latin1, and in GSM are greek
	 * characters that are unrepresentable in latin1.  So we let them
	 * map to self, to create a way to specify them. */
	 16, NRP,  18,  19,  20,  21,  22,  23,       /* 16 - 23 */
	 24,  25,  26, NRP, NRP, NRP, NRP, NRP,       /* 24 - 31 */
	/* $ maps to 2 */
	' ', '!', '"', '#',   2, '%', '&', '\'',      /* 32 - 39 */
	'(', ')', '*', '+', ',', '-', '.', '/',       /* 40 - 47 */
	'0', '1', '2', '3', '4', '5', '6', '7',       /* 48 - 55 */
	'8', '9', ':', ';', '<', '=', '>', '?',       /* 56 - 63 */
	/* @ maps to 0 */
	  0, 'A', 'B', 'C', 'D', 'E', 'F', 'G',       /* 64 - 71 */
	'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',       /* 72 - 79 */
	'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W',       /* 80 - 87 */
	/* [ is an escaped < */
	/* \ is an escaped / */
	/* ] is an escaped > */
	/* ^ is an escaped Greek Lambda */
	/* _ maps to 17 */
	'X', 'Y', 'Z', -60, -47, -62, -20,  17,       /* 88 - 95 */
	/* The backquote cannot be represented at all */
	NRP, 'a', 'b', 'c', 'd', 'e', 'f', 'g',       /* 96 - 103 */
	'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o',       /* 104 - 111 */
	'p', 'q', 'r', 's', 't', 'u', 'v', 'w',       /* 112 - 119 */
	/* { is an escaped ( */
	/* | is an escaped inverted ! */
	/* } is an escaped ) */
	/* ~ is an escaped = */
	'x', 'y', 'z', -40, -64, -41, -61, NRP,       /* 120 - 127 */
	NRP, NRP, NRP, NRP, NRP, NRP, NRP, NRP,       /* 128 - 135 */
	NRP, NRP, NRP, NRP, NRP, NRP, NRP, NRP,       /* 136 - 143 */
	NRP, NRP, NRP, NRP, NRP, NRP, NRP, NRP,       /* 144 - 151 */
	NRP, NRP, NRP, NRP, NRP, NRP, NRP, NRP,       /* 152 - 159 */

	/* 160 - 167 */
	' ',
	 64, /* Inverted ! */
	'c', /* approximation of cent marker */
	  1, /* Pounds sterling */
	 36, /* International currency symbol */
	  3, /* Yen */
	 64, /* approximate broken bar as inverted ! */
	 95, /* Section marker */

	/* 168 - 175 */
	'"', /* approximate dieresis */
	'C', /* approximate copyright marker */
	'a', /* approximate ordfeminine */
	'<', /* approximate french << */
	'!', /* approximate logical not sign */
	'-', /* approximate hyphen */
	'R', /* approximate registered marker */
	'-', /* approximate macron */

	/* 176 - 183 */
	'o', /* approximate degree marker */
	NRP, /* plusminus */
	'2', /* approximate superscript 2 */
	'3', /* approximate superscript 3 */
	'\'', /* approximate acute accent */
	'u', /* approximate greek mu */
	NRP, /* paragraph marker */
	'.', /* approximate bullet */

	/* 184 - 191 */
	',', /* approximate cedilla */
	'i', /* approximate dotless i */
	'o', /* approximate ordmasculine */
	'>', /* approximate french >> */
	NRP, /* onequarter */
	NRP, /* onehalf */
	NRP, /* threequarters */
	 96, /* Inverted ? */
	
	/* 192 - 199 */
	'A', /* approximate A grave */
	'A', /* approximate A acute */
	'A', /* approximate A circumflex */
	'A', /* approximate A tilde */
	 91, /* A dieresis */
	 14, /* A ring */
	 28, /* AE ligature */
	  9, /* C cedilla */

	/* 200 - 207 */
	'E', /* approximate E grave */
	 31, /* E acute */
	'E', /* approximate E circumflex */
	'E', /* approximate E dieresis */
	'I', /* approximate I grave */
	'I', /* approximate I acute */
	'I', /* approximate I circumflex */
	'I', /* approximate I dieresis */

	/* 208 - 215 */
	NRP, /* Eth */
	 93, /* N tilde */
	'O', /* approximate O grave */
	'O', /* approximate O acute */
	'O', /* approximate O circumflex */
	'O', /* approximate O tilde */
	 92, /* O dieresis */
	'x', /* approximate multiplication sign */

	/* 216 - 223 */
	 11, /* O slash */
	'U', /* approximate U grave */
	'U', /* approximate U acute */
	'U', /* approximate U circumflex */
	 94, /* U dieresis */
	'Y', /* approximate Y acute */
	NRP, /* approximate Thorn */
	 30, /* german double-s */

	/* 224 - 231 */
	127, /* a grave */
	'a', /* approximate a acute */
	'a', /* approximate a circumflex */
	'a', /* approximate a tilde */
	123, /* a dieresis */
	 15, /* a ring */
	 29, /* ae ligature */
	  9, /* approximate c cedilla as C cedilla */

	/* 232 - 239 */
	  4, /* e grave */
	  5, /* e acute */
	'e', /* approximate e circumflex */
	'e', /* approximate e dieresis */
	  7, /* i grave */
	'i', /* approximate i acute */
	'i', /* approximate i circumflex */
	'i', /* approximate i dieresis */

	/* 240 - 247 */
	NRP, /* eth */
	125, /* n tilde */
	  8, /* o grave */
	'o', /* approximate o acute */
	'o', /* approximate o circumflex */
	'o', /* approximate o tilde */
	124, /* o dieresis */
	NRP, /* division sign */

	/* 248 - 255 */
	 12, /* o slash */
	  6, /* u grave */
	'u', /* approximate u acute */
	'u', /* approximate u circumflex */
	126, /* u dieresis */
	'y', /* approximate y acute */
	NRP, /* thorn */
	'y', /* approximate y dieresis */
};


void charset_gsm_to_latin1(Octstr *ostr)
{
    long pos, len;

    len = octstr_len(ostr);
    for (pos = 0; pos < len; pos++) {
	int c, new, i;

	c = octstr_get_char(ostr, pos);
	if (c == 27 && pos + 1 < len) {
	    /* GSM escape code.  Delete it, then process the next
             * character specially. */
	    octstr_delete(ostr, pos, 1);
	    len--;
	    c = octstr_get_char(ostr, pos);
	    for (i = 0; gsm_escapes[i].gsmesc >= 0; i++) {
		if (gsm_escapes[i].gsmesc == c)
	  	    break;
	    }
	    if (gsm_escapes[i].gsmesc == c)
		new = gsm_escapes[i].latin1;
	    else if (c < 128)
		new = gsm_to_latin1[c];
	    else
		continue;
	} else if (c < 128) {
            new = gsm_to_latin1[c];
	} else {
	    continue;
	}
	if (new != c)
	    octstr_set_char(ostr, pos, new);
    }
}

void charset_latin1_to_gsm(Octstr *ostr)
{
    long pos, len;
    int c, new;
    unsigned char esc = 27;

    len = octstr_len(ostr);
    for (pos = 0; pos < len; pos++) {
	c = octstr_get_char(ostr, pos);
	gw_assert(c >= 0);
	gw_assert(c <= 256);
	new = latin1_to_gsm[c];
	if (new < 0) {
   	     /* Escaped GSM code */
	    octstr_insert_data(ostr, pos, &esc, 1);
	    pos++;
	    len++;
	    new = -new;
	}
	if (new != c)
	    octstr_set_char(ostr, pos, new);
    }
}

int charset_gsm_truncate(Octstr *gsm, long max)
{
    if (octstr_len(gsm) > max) {
	/* If the last GSM character was an escaped character,
	 * then chop off the escape as well as the character. */
	if (octstr_get_char(gsm, max - 1) == 27)
  	    octstr_truncate(gsm, max - 1);
	else
	    octstr_truncate(gsm, max);
	return 1;
    }
    return 0;
}
