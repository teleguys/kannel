/*
 * wml_definitions.h - definitions unique to WML compiler
 *
 * This file contains fefinitions for global tokens and structures containing 
 * element and attribute tokens for the code page 1.
 *
 *
 * Tuomas Luttinen for Wapit Ltd.
 */

/***********************************************************************
 * Declarations of global variables. 
 */

/*
 * Elements as defined by tag code page 0.
 */

static
wml_table_t wml_elements[] = {
  { "wml", 0x3F },
  { "card", 0x27 },
  { "do", 0x28 },
  { "onevent", 0x33 },
  { "head", 0x2C },
  { "template", 0x3B },
  { "access", 0x23 },
  { "meta", 0x30 },
  { "go", 0x2B },
  { "prev", 0x32 },
  { "refresh", 0x36 },
  { "noop", 0x31 },
  { "postfield", 0x21 },
  { "setvar", 0x3E },
  { "select", 0x37 },
  { "optgroup", 0x34 },
  { "option", 0x35 },
  { "input", 0x2F },
  { "fieldset", 0x2A },
  { "timer", 0x3C },
  { "img", 0x2E },
  { "anchor", 0x22 },
  { "a", 0x1C },
  { "table", 0x1F },
  { "tr", 0x1E },
  { "td", 0x1D },
  { "em", 0x29 },
  { "strong", 0x39 },
  { "b", 0x24 },
  { "i", 0x2D },
  { "u", 0x3D },
  { "big", 0x25 },
  { "small", 0x38 },
  { "p", 0x20 },
  { "br", 0x26 },
  { NULL }
};


/*
 * Attributes as defined by attribute code page 0.
 */

static
wml_table3_t wml_attributes[] = {
  { "accept-charset", NULL, 0x05 },
  { "align", NULL, 0x52 },
  { "align", "bottom", 0x06 },
  { "align", "center", 0x07 },
  { "align", "left", 0x08 },
  { "align", "middle", 0x09 },
  { "align", "right", 0x0A },
  { "align", "top", 0x0B },
  { "alt", NULL, 0x0C },
  { "class", NULL, 0x54 },
  { "columns", NULL, 0x53 },
  { "content", NULL, 0x0D },
  { "content", "application/vnd.wap.wmlc;charset=", 0x5C },
  { "domain", NULL, 0x0F },
  { "emptyok", "false", 0x10 },
  { "emptyok", "true", 0x11 },
  { "format", NULL, 0x12 },
  { "forua", "false", 0x56 },
  { "forua", "true", 0x57 },
  { "height", NULL, 0x13 },
  { "href", NULL, 0x4A },
  { "href", "http://", 0x4B },
  { "href", "https://", 0x4C },
  { "hspace", NULL, 0x14 },
  { "http-equiv", NULL, 0x5A },
  { "http-equiv", "Content-Type", 0x5B },
  { "http-equiv", "Expires", 0x5D },
  { "id", NULL, 0x55 },
  { "ivalue", NULL, 0x15 },
  { "iname", NULL, 0x16 },
  { "label", NULL, 0x18 },
  { "localsrc", NULL, 0x19 },
  { "maxlength", NULL, 0x1A },
  { "method", "get", 0x1B },
  { "method", "post", 0x1C },
  { "mode", "nowrap", 0x1D },
  { "mode", "wrap", 0x1E },
  { "multiple", "false", 0x1F },
  { "multiple", "true", 0x20 },
  { "name", NULL, 0x21 },
  { "newcontext", "false", 0x22 },
  { "newcontext", "true", 0x23 },
  { "onenterbackward", NULL, 0x25 },
  { "onenterforward", NULL, 0x26 },
  { "onpick", NULL, 0x24 },
  { "ontimer", NULL, 0x27 },
  { "optional", "false", 0x28 },
  { "optional", "true", 0x29 },
  { "path", NULL, 0x2A },
  { "scheme", NULL, 0x2E },
  { "sendreferer", "false", 0x2F },
  { "sendreferer", "true", 0x30 },
  { "size", NULL, 0x31 },
  { "src", NULL, 0x32 },
  { "src", "http://", 0x58 },
  { "src", "https://", 0x59 },
  { "ordered", "true", 0x33 },
  { "ordered", "false", 0x34 },
  { "tabindex", NULL, 0x35 },
  { "title", NULL, 0x36 },
  { "type", NULL, 0x37 },
  { "type", "accept", 0x38 },
  { "type", "delete", 0x39 },
  { "type", "help", 0x3A },
  { "type", "password", 0x3B },
  { "type", "onpick", 0x3C },
  { "type", "onenterbackward", 0x3D },
  { "type", "onenterforward", 0x3E },
  { "type", "ontimer", 0x3F },
  { "type", "options", 0x45 },
  { "type", "prev", 0x46 },
  { "type", "reset", 0x47 },
  { "type", "text", 0x48 },
  { "type", "vnd.", 0x49 },
  { "value", NULL, 0x4D },
  { "vspace", NULL, 0x4E },
  { "width", NULL, 0x4F },
  { "xml:lang", NULL, 0x50 },
  { NULL }
};


/*
 * Attribute value codes.
 */

static
wml_table_t wml_attribute_values[] = {
  { "accept", 0x89 },
  { "bottom", 0x8A },
  { "clear", 0x8B },
  { "delete", 0x8C },
  { "help", 0x8D },
  { "middle", 0x93 },
  { "nowrap", 0x94 },
  { "onenterbackward", 0x96 },
  { "onenterforward", 0x97 },
  { "onpick", 0x95 },
  { "ontimer", 0x98 },
  { "options", 0x99 },
  { "password", 0x9A },
  { "reset", 0x9B },
  { "text", 0x9D },
  { "top", 0x9E },
  { "unknown", 0x9F },
  { "wrap", 0xA0 },
  { NULL }
};

/*
 * URL value codes.
 */

static
wml_table_t wml_URL_values[] = {
  { "www.", 0xA1 },
  { ".com/", 0x85 },
  { ".edu/", 0x86 },
  { ".net/", 0x87 },
  { ".org/", 0x88 },
  { NULL }
};














