/*
 * cfg.c - configuration file handling
 *
 * Lars Wirzenius
 */


#include "gwlib/gwlib.h"


struct CfgGroup {
    Octstr *name;
    Dict *vars;
};


static CfgGroup *create_group(void)
{
    CfgGroup *grp;
    
    grp = gw_malloc(sizeof(*grp));
    grp->name = NULL;
    grp->vars = dict_create(64, octstr_destroy_item);
    return grp;
}

static void destroy_group(void *arg)
{
    CfgGroup *grp;
    
    if (arg != NULL) {
	grp = arg;
	octstr_destroy(grp->name);
	dict_destroy(grp->vars);
	gw_free(grp);
    }
}


static void destroy_group_list(void *arg)
{
    list_destroy(arg, destroy_group);
}


static void set_group_name(CfgGroup *grp, Octstr *name)
{
    octstr_destroy(grp->name);
    grp->name = octstr_duplicate(name);
}


struct Cfg {
    Octstr *filename;
    Dict *single_groups;
    Dict *multi_groups;
};


static int is_allowed_in_group(Octstr *group, Octstr *variable)
{
    Octstr *groupstr;
    
    groupstr = octstr_imm("group");

    #define OCTSTR(name) \
    	if (octstr_compare(octstr_imm(#name), variable) == 0) \
	    return 1;
    #define SINGLE_GROUP(name, fields) \
    	if (octstr_compare(octstr_imm(#name), group) == 0) { \
	    if (octstr_compare(groupstr, variable) == 0) \
		return 1; \
	    fields \
	    return 0; \
	}
    #define MULTI_GROUP(name, fields) \
    	if (octstr_compare(octstr_imm(#name), group) == 0) { \
	    if (octstr_compare(groupstr, variable) == 0) \
		return 1; \
	    fields \
	    return 0; \
	}
    #include "cfg.def"

    return 0;
}


static int is_single_group(Octstr *query)
{
    #define OCTSTR(name)
    #define SINGLE_GROUP(name, fields) \
    	if (octstr_compare(octstr_imm(#name), query) == 0) \
	    return 1;
    #define MULTI_GROUP(name, fields) \
    	if (octstr_compare(octstr_imm(#name), query) == 0) \
	    return 0;
    #include "cfg.def"
    return 0;
}


static int add_group(Cfg *cfg, CfgGroup *grp)
{
    Octstr *groupname;
    Octstr *name;
    List *names;
    List *list;
    
    groupname = cfg_get(grp, octstr_imm("group"));
    if (groupname == NULL) {
	error(0, "Group doens not contain variable 'group'.");
    	return -1;
    }
    set_group_name(grp, groupname);

    names = dict_keys(grp->vars);
    while ((name = list_extract_first(names)) != NULL) {
	if (!is_allowed_in_group(groupname, name)) {
	    error(0, "Group '%s' may not contain field '%s'.",
		  octstr_get_cstr(groupname), octstr_get_cstr(name));
	    octstr_destroy(name);
	    octstr_destroy(groupname);
	    list_destroy(names, octstr_destroy_item);
	    return -1;
	}
	octstr_destroy(name);
    }
    list_destroy(names, NULL);

    if (is_single_group(groupname))
    	dict_put(cfg->single_groups, groupname, grp);
    else {
	list = dict_get(cfg->multi_groups, groupname);
	if (list == NULL) {
	    list = list_create();
	    dict_put(cfg->multi_groups, groupname, list);
	}
    	list_append(list, grp);
    }

    octstr_destroy(groupname);
    return 0;
}


Cfg *cfg_create(Octstr *filename)
{
    Cfg *cfg;
    
    cfg = gw_malloc(sizeof(*cfg));
    cfg->filename = octstr_duplicate(filename);
    cfg->single_groups = dict_create(64, destroy_group);
    cfg->multi_groups = dict_create(64, destroy_group_list);
    return cfg;
}


void cfg_destroy(Cfg *cfg)
{
    if (cfg != NULL) {
	octstr_destroy(cfg->filename);
	dict_destroy(cfg->single_groups);
	dict_destroy(cfg->multi_groups);
	gw_free(cfg);
    }
}


static void parse_value(Octstr *value)
{
    Octstr *temp;
    long len;
    int c;
    
    octstr_strip_blanks(value);

    len = octstr_len(value);
    if (octstr_get_char(value, 0) != '"' || 
        octstr_get_char(value, len - 1) != '"')
	return;

    octstr_delete(value, len - 1, 1);
    octstr_delete(value, 0, 1);

    temp = octstr_duplicate(value);
    octstr_truncate(value, 0);
    
    while (octstr_len(temp) > 0) {
	c = octstr_get_char(temp, 0);
	octstr_delete(temp, 0, 1);
	
    	if (c != '\\' || octstr_len(temp) == 0)
	    octstr_append_char(value, c);
	else {
	    c = octstr_get_char(temp, 0);
	    octstr_delete(temp, 0, 1);

	    switch (c) {
    	    case '\\':
    	    case '"':
	    	octstr_append_char(value, c);
	    	break;
		
    	    default:
	    	octstr_append_char(value, '\\');
	    	octstr_append_char(value, c);
		break;
	    }
	}
    }
    
    octstr_destroy(temp);
}


int cfg_read(Cfg *cfg)
{
    Octstr *os;
    Octstr *line;
    List *lines;
    Octstr *name;
    Octstr *value;
    CfgGroup *grp;
    long equals;
    long lineno;
    long error_lineno;
    
    os = octstr_read_file(octstr_get_cstr(cfg->filename));
    if (os == NULL)
    	return -1;

    lines = octstr_split(os, octstr_imm("\n"));
    octstr_destroy(os);
    
    grp = NULL;
    lineno = 0;
    error_lineno = 0;
    while (error_lineno == 0 && (line = list_extract_first(lines)) != NULL) {
	++lineno;
	octstr_strip_blanks(line);
    	if (octstr_len(line) == 0) {
	    if (grp != NULL && add_group(cfg, grp) == -1) {
		error_lineno = lineno;
		destroy_group(grp);
	    }
	    grp = NULL;
	} else if (octstr_get_char(line, 0) != '#') {
	    equals = octstr_search_char(line, '=', 0);
	    if (equals == -1) {
		error(0, "An equals sign ('=') is missing.");
	    	error_lineno = lineno;
	    } else {
		name = octstr_copy(line, 0, equals);
		octstr_strip_blanks(name);
		value = octstr_copy(line, equals + 1, octstr_len(line));
		parse_value(value);
    	    	if (grp == NULL)
		    grp = create_group();
		cfg_set(grp, name, value);
		octstr_destroy(name);
		octstr_destroy(value);
	    }
	}

	octstr_destroy(line);
    }

    if (grp != NULL && add_group(cfg, grp) == -1) {
    	error_lineno = 1;
	destroy_group(grp);
    }

    list_destroy(lines, octstr_destroy_item);

    if (error_lineno != 0) {
	error(0, "Error found on %ld of file %s.", 
	      lineno, octstr_get_cstr(cfg->filename));
	return -1;
    }

    return 0;
}


CfgGroup *cfg_get_single_group(Cfg *cfg, Octstr *name)
{
    return dict_get(cfg->single_groups, name);
}


List *cfg_get_multi_group(Cfg *cfg, Octstr *name)
{
    List *list, *copy;
    long i;
    
    list = dict_get(cfg->multi_groups, name);
    if (list == NULL)
    	return NULL;

    copy = list_create();
    for (i = 0; i < list_len(list); ++i)
    	list_append(copy, list_get(list, i));
    return copy;
}


Octstr *cfg_get_group_name(CfgGroup *grp)
{
    return octstr_duplicate(grp->name);
}


Octstr *cfg_get_real(CfgGroup *grp, Octstr *varname, const char *file, 
    	    	     long line, const char *func)
{
    Octstr *os;

    if (grp->name != NULL && !is_allowed_in_group(grp->name, varname))
    	panic(0, "Trying to fetch variable `%s' in group `%s', not allowed.",
	      octstr_get_cstr(varname), octstr_get_cstr(grp->name));

    os = dict_get(grp->vars, varname);
    if (os == NULL)
    	return NULL;
    return gw_claim_area_for(octstr_duplicate(os), file, line, func);
}


int cfg_get_integer(long *n, CfgGroup *grp, Octstr *varname)
{
    Octstr *os;
    int ret;
    
    os = cfg_get(grp, varname);
    if (os == NULL)
    	return -1;
    if (octstr_parse_long(n, os, 0, 0) == -1)
    	ret = -1;
    else
    	ret = 0;
    octstr_destroy(os);
    return ret;
}


int cfg_get_bool(int *n, CfgGroup *grp, Octstr *varname)
{
    Octstr *os;

    os = cfg_get(grp, varname);
    if (os == NULL) {
	*n = 0;
    	return -1;
    }
    if (octstr_case_compare(os, octstr_imm("true")) == 0
	|| octstr_case_compare(os, octstr_imm("yes")) == 0
	|| octstr_case_compare(os, octstr_imm("on")) == 0
	|| octstr_case_compare(os, octstr_imm("1")) == 0)
    {	    
	*n = 1;
    } else if (octstr_case_compare(os, octstr_imm("false")) == 0
	|| octstr_case_compare(os, octstr_imm("no")) == 0
	|| octstr_case_compare(os, octstr_imm("off")) == 0
	|| octstr_case_compare(os, octstr_imm("0")) == 0)
    {
	*n = 0;
    }
    else {
	*n = 1;
	warning(0, "bool variable set to strange value, assuming 'true'");
    }
    octstr_destroy(os);
    return 0;
}


List *cfg_get_list(CfgGroup *grp, Octstr *varname)
{
    Octstr *os;
    List *list;
    
    os = cfg_get(grp, varname);
    if (os == NULL)
    	return NULL;

    list = octstr_split_words(os);
    octstr_destroy(os);
    return list;
}


void cfg_set(CfgGroup *grp, Octstr *varname, Octstr *value)
{
    dict_put(grp->vars, varname, octstr_duplicate(value));
}


static void dump_group(CfgGroup *grp)
{
    List *names;
    Octstr *name;
    Octstr *value;

    if (grp->name == NULL)
	debug("gwlib.cfg", 0, "  dumping group (name not set):");
    else
	debug("gwlib.cfg", 0, "  dumping group (%s):",
	      octstr_get_cstr(grp->name));
    names = dict_keys(grp->vars);
    while ((name = list_extract_first(names)) != NULL) {
	value = cfg_get(grp, name);
	debug("gwlib.cfg", 0, "    <%s> = <%s>", 
	      octstr_get_cstr(name),
	      octstr_get_cstr(value));
    	octstr_destroy(value);
    	octstr_destroy(name);
    }
    list_destroy(names, NULL);
}


void cfg_dump(Cfg *cfg)
{
    CfgGroup *grp;
    List *list;
    List *names;
    Octstr *name;

    debug("gwlib.cfg", 0, "Dumping Cfg %p", (void *) cfg);
    debug("gwlib.cfg", 0, "  filename = <%s>", 
    	  octstr_get_cstr(cfg->filename));

    names = dict_keys(cfg->single_groups);
    while ((name = list_extract_first(names)) != NULL) {
	grp = cfg_get_single_group(cfg, name);
	if (grp != NULL)
	    dump_group(grp);
    	octstr_destroy(name);
    }
    list_destroy(names, NULL);

    names = dict_keys(cfg->multi_groups);
    while ((name = list_extract_first(names)) != NULL) {
	list = cfg_get_multi_group(cfg, name);
	while ((grp = list_extract_first(list)) != NULL)
	    dump_group(grp);
	list_destroy(list, NULL);
    	octstr_destroy(name);
    }
    list_destroy(names, NULL);

    debug("gwlib.cfg", 0, "Dump ends.");
}
