/* wsp_pdu.h - definitions for unpacked WTP protocol data units
 *
 * This file generates a structure definition and some function
 * declarations from wtp_pdu.def, using preprocessor magic.
 *
 * Richard Braakman
 */

#ifndef WSP_PDU_H
#define WSP_PDU_H

#include "gwlib/gwlib.h"

/* The Get and Post PDUs contain a "subtype" field.  Sometimes we
 * have to reconstruct the full method number.  For methods encoded
 * in Get PDUs, this is GET_METHODS + subtype.  For methods encoded
 * in Post PDUs, this is POST_METHODS + subtype. */
enum {
    GET_METHODS = 0x40,
    POST_METHODS = 0x60
};

/* Enumerate the symbolic names of the PDUs */
enum wsp_pdu_types {
#define PDU(name, docstring, fields, is_valid) name,
#include "wsp_pdu.def"
#undef PDU
};

struct wsp_pdu {
	int type;

	union {
/* For each PDU, declare a structure with its fields, named after the PDU */
#define PDU(name, docstring, fields, is_valid) struct name { fields } name;
#define UINT(field, docstring, bits) unsigned long field;
#define UINTVAR(field, docstring) unsigned long field;
#define OCTSTR(field, docstring, lengthfield) Octstr *field;
#define REST(field, docstring) Octstr *field;
#define TYPE(bits, value)
#define RESERVED(bits)
#define TPI(confield)
#include "wsp_pdu.def"
#undef TPI
#undef RESERVED
#undef TYPE
#undef REST
#undef OCTSTR
#undef UINTVAR
#undef UINT
#undef PDU
	} u;
};
typedef struct wsp_pdu WSP_PDU;

WSP_PDU *wsp_pdu_create(int type);
WSP_PDU *wsp_pdu_unpack(Octstr *data);
Octstr *wsp_pdu_pack(WSP_PDU *pdu);
void wsp_pdu_dump(WSP_PDU *pdu, int level);
void wsp_pdu_destroy(WSP_PDU *pdu);

#endif