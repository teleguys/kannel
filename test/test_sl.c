/*
 * test_sl.c: A simple program to test sl tokenizer
 *
 * By Aarno Syv�nen for Wiral Ltd
 */

#include <stdio.h>

#include "gwlib/gwlib.h"
#include "gw/wap_push_sl_compiler.h"

Octstr *charset = NULL;
Octstr *file_name = NULL;

static void help (void)
{
    info(0, "Usage test_sl [option] sl_source");
    info(0, "where options are");
    info(0, "-h print this text");
    info(0, "-f file output binary to the file");
    info(0, "-c charset charset given by http");
    info(0, "-v level set log level for stderr logging");
}

int main(int argc, char **argv)
{
    int opt,
        file,
        have_charset,
        ret;
    FILE *fp;
    Octstr *output,
           *sl_doc,
           *sl_binary;

    gwlib_init();
    file = 0;
    have_charset = 0;
    fp = NULL;

    while ((opt = getopt(argc, argv, "hf:c:v:")) != EOF) {
        switch (opt) {
        case 'h':
	    help();
            exit(1);
	break;

        case 'f':
	    file = 1;
	    file_name = octstr_create(optarg);
            fp = fopen(optarg, "a");
            if (fp == NULL)
	        panic(0, "Cannot open output file");
	break;

        case 'c':
	    have_charset = 1;
	    charset = octstr_create(optarg);
	break;

        case 'v':
	    log_set_output_level(atoi(optarg));
	break;

        case '?':
        default:
	    error(0, "Invalid option %c", opt);
            help();
            panic(0, "Stopping");
	break;
        }
    }

    if (optind >= argc) {
        error(0, "Missing arguments");
        help();
        panic(0, "Stopping");
    }

    sl_doc = octstr_read_file(argv[optind]);
    if (sl_doc == NULL)
        panic(0, "Cannot read the sl document");

    if (!have_charset)
        charset = NULL;
    ret = sl_compile(sl_doc, charset, &sl_binary);
    output = octstr_format("%s", "sl compiler returned %d\n", ret);

    if (ret == 0) {
        if (fp == NULL)
	    fp = stdout;
        octstr_append(output, octstr_imm("content being\n"));
        octstr_append(output, sl_binary);
    }

    if (file)
        octstr_pretty_print(fp, output);
    else {
        debug("test.sl", 0, "sl binary was");
        octstr_dump(sl_binary, 0);
    }

    if (have_charset)
        octstr_destroy(charset);
    if (file) {
        fclose(fp);
        octstr_destroy(file_name);
    }
    
    octstr_destroy(sl_doc);
    octstr_destroy(sl_binary);
    octstr_destroy(output);
    gwlib_shutdown();
    return 0;
}




