/*
 *
 * ws.h
 *
 * Author: Markku Rossi <mtr@iki.fi>
 *
 * Copyright (c) 1999-2000 Markku Rossi, etc.
 *		 All rights reserved.
 *
 * Public header file for the WMLScript compiler library.
 *
 */

#ifndef WS_H
#define WS_H

#include <wsutf8.h>

/********************* Creating and destroying compiler *****************/

/* A callback function of this type is called to output compiler's
   diagnostic and, error, and warning messages.  The argument `data'
   has `len' bytes of data that should be output somehow to user.  The
   argument `context' is the user-specified context data for the
   callback function. */
typedef void (*WsIOProc)(const char *data, size_t len, void *context);

/* A callback function of this type is called for each `use meta name'
   and `use meta http equiv' pragma, found from the current
   compilation unit.  The arguments `property_name', `content', and
   `scheme' are the meta-body arguments of the pragma.  They can be
   manipulated with the functions of the `wsutf8.h' module.  The
   argument `scheme' can have the value NULL if the pragma did not
   specify it.  The string arguments belong to the WMLScript compiler
   and you should not modify or free them.  The argument `context' is
   the user-specified context data for the callback function. */
typedef void (*WsPragmaMetaProc)(const WsUtf8String *property_name,
				 const WsUtf8String *content,
				 const WsUtf8String *scheme,
				 void *context);

/* Parameters for a WMLScript copiler. */
struct WsCompilerParamsRec
{
  /* Features. */

  /* Store string constants in ISO-8859/1 (ISO latin1) format.  The
     default format is UTF-8.  This option makes a bit smaller
     byte-code files but it loses information for non-latin1
     languages. */
  unsigned int use_latin1_strings : 1;


  /* Warning flags. */

  /* Warn if a standard library function is called with mismatching
     argument types. */
  unsigned int warn_stdlib_type_mismatch : 1;


  /* Optimization flags. */

  /* Do not perform constant folding. */
  unsigned int no_opt_constant_folding : 1;

  /* Do not sort byte-code functions by their usage counts. */
  unsigned int no_opt_sort_bc_functions : 1;

  /* Do not optimize jumps to jump instructions to jump directly to
     the target label of the next instruction. */
  unsigned int no_opt_jumps_to_jumps : 1;

  /* Do not optimize jumps to the next instruction. */
  unsigned int no_opt_jumps_to_next_instruction : 1;

  /* Do not remove unreachable code. */
  unsigned int no_opt_dead_code : 1;

  /* Perform expensive optimizations which require liveness
     analyzation of the local variables. */
  unsigned int opt_analyze_variable_liveness;


  /* Output flags. */

  /* Print symbolic assembler to the stdout. */
  unsigned int print_symbolic_assembler : 1;

  /* Disassemble the resulting byte-code instructions. */
  unsigned int print_assembler : 1;

  /* Function pointers to receive standard output and error messages.
     If these are unset, the outputs are directed to the system's
     standard output and error streams. */

  /* Standard output. */
  WsIOProc stdout_cb;
  void *stdout_cb_context;

  /* Standard error. */
  WsIOProc stderr_cb;
  void *stderr_cb_context;

  /* A callback function which is called for each `use meta name'
     pragma, found from the current compilation unit. */
  WsPragmaMetaProc meta_name_cb;
  void *meta_name_cb_context;

  /* A callback function which is called for each `use meta http
     equiv' pragma, found from the current compilation unit. */
  WsPragmaMetaProc meta_http_equiv_cb;
  void *meta_http_equiv_cb_context;
};

typedef struct WsCompilerParamsRec WsCompilerParams;

/* A compiler handle. */
typedef struct WsCompilerRec *WsCompilerPtr;

/* Create a new WMLScript compiler.  The argument `params' specifies
   initialization parameters for the compiler.  If the argument
   `params' is NULL or any of its fiels have value 0 or NULL, the
   default values will be used for the parameter.  The function takes
   a copy of the value of the `params' argument.  You can free it
   after this call.  The function returns NULL if the operation fails
   (out of memory). */
WsCompilerPtr ws_create(const WsCompilerParams *params);

/* Destroy the WMLScript compiler `compiler' and free all resources it
   has allocated. */
void ws_destroy(WsCompilerPtr compiler);

/********************* Compiling WMLScript ******************************/

/* Returns codes for the compiler functions. */
typedef enum
{
  /* Successful termination */
  WS_OK,

  /* The compiler ran out of memory. */
  WS_ERROR_OUT_OF_MEMORY,

  /* The input was not syntactically correct. */
  WS_ERROR_SYNTAX,

  /* The input was not semantically correct. */
  WS_ERROR_SEMANTIC,

  /* IO error. */
  WS_ERROR_IO,

  /* A generic `catch-all' error code.  This should not be used.  More
     descriptive error messages should be generated instead. */
  WS_ERROR
} WsResult;

/* Compile the WMLScript input file `input' with the compiler
   `compiler' and save the generated byte-code output to the file
   `output'.  The argument `input_name' is the name of the input file
   `input'.  It is used in error messages. The function returns a
   success code that describes the result of the compilation.  The
   output file `output' is modified only if the result code is
   `WS_OK'. */
WsResult ws_compile_file(WsCompilerPtr compiler, const char *input_name,
			 FILE *input, FILE *output);

/* Compile the `input_len' bytes of WMLScript data in `input' with the
   compiler `compiler'.  The resulting byte-code is returned in
   `output_return' and its length is returned in `output_len_return'.
   The argument `input_name' is the name of the input data
   `input_data'.  It is used in error messages.  The function returns
   a success code that describes the result of the compilation.  The
   output in `output_return' is valid only if the result code is
   `WS_OK'.  The byte-code, returned in `output_return' must be freed
   with the ws_free_byte_code() function after it is not needed
   anymore.  It is a fatal error to free it with any other function,
   like free(). */
WsResult ws_compile_data(WsCompilerPtr compiler, const char *input_name,
			 const unsigned char *input, size_t input_len,
			 unsigned char **output_return,
			 size_t *output_len_return);

/* Free the byte-code buffer `byte_code', returned by
   ws_compiler_data() function.  The byte-code `byte_code' must not be
   used after this function has been called. */
void ws_free_byte_code(unsigned char *byte_code);

/* Convert the result code `result' into human readable 7 bit ASCII
   string. */
const char *ws_result_to_string(WsResult result);

#endif /* not WS_H */
