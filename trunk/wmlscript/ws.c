/*
 *
 * ws.c
 *
 * Author: Markku Rossi <mtr@iki.fi>
 *
 * Copyright (c) 1999-2000 Markku Rossi, etc.
 *		 All rights reserved.
 *
 * Public entry points to the WMLScript compiler and the main compile
 * function.
 *
 */

#include <wsint.h>
#include <ws.h>
#include <wsstree.h>
#include <wsasm.h>

/********************* Types and definitions ****************************/

#define WS_CHECK_COMPILE_ERROR()				\
  do {								\
    if (compiler->errors != 0)					\
      {								\
	if (compiler->errors & WS_ERROR_B_MEMORY)		\
	  result = WS_ERROR_OUT_OF_MEMORY;			\
	else if (compiler->errors & WS_ERROR_B_SYNTAX)		\
	  result = WS_ERROR_SYNTAX;				\
	else if (compiler->errors & WS_ERROR_B_SEMANTIC)	\
	  result = WS_ERROR_SEMANTIC;				\
	else							\
	  /* This shouldn't happen. */				\
	  result = WS_ERROR;					\
	goto out;						\
      }								\
  } while (0);

/********************* Static variables *********************************/

/* Human readable names for the result codes. */
static struct
{
  WsResult code;
  char *description;
} result_codes[] =
{
  {WS_OK,			"success"},
  {WS_ERROR_OUT_OF_MEMORY,	"out of memory"},
  {WS_ERROR_SYNTAX,		"syntax error"},
  {WS_ERROR_SEMANTIC,		"compile error"},
  {WS_ERROR_IO,			"IO error"},
  {WS_ERROR,			"error"},

  {0, NULL},
};

/********************* Prototypes for static functions ******************/

/* Compile the input stream `input' with the compiler `compiler' and
   return the byte-code in `output_return' and its length in
   `output_len_return'.  The function returns a WsResult error code
   describing the success of the compilation. */
static WsResult compile_stream(WsCompilerPtr compiler,
			       const char *input_name, WsStream *input,
			       unsigned char **output_return,
			       size_t *output_len_return);

/* The default I/O function to send the output to stdout and stderr.
   The argument `context' must be a `FILE *' file to which the output
   is written. */
static void std_io(const char *data, size_t len, void *context);

/********************* Global functions *********************************/

WsCompilerPtr
ws_create(const WsCompilerParams *params)
{
  WsCompilerPtr compiler = ws_calloc(1, sizeof(*compiler));

  if (compiler == NULL)
    return NULL;

  /* Store user params if specified. */
  if (params)
    memcpy(&compiler->params, params, sizeof(*params));

  /* Basic initialization. */

  compiler->magic = 0xfefe0101;

  if (compiler->params.stdout_cb == NULL)
    {
      compiler->params.stdout_cb = std_io;
      compiler->params.stdout_cb_context = stdout;
    }
  if (compiler->params.stderr_cb == NULL)
    {
      compiler->params.stderr_cb = std_io;
      compiler->params.stderr_cb_context = stderr;
    }

  return compiler;
}


void
ws_destroy(WsCompilerPtr compiler)
{
  if (compiler == NULL)
    return;

  ws_free(compiler);

#if WS_MEM_DEBUG
  if (ws_has_leaks())
    ws_dump_blocks();
#endif /* WS_MEM_DEBUG */
}


WsResult
ws_compile_file(WsCompilerPtr compiler, const char *input_name, FILE *input,
		FILE *output)
{
  WsResult result;
  WsStream *stream;
  unsigned char *bc;
  size_t bc_len;

  /* Initialize the input stream. */
  stream = ws_stream_new_file(input, WS_FALSE, WS_FALSE);
  if (stream == NULL)
    return WS_ERROR_OUT_OF_MEMORY;

  result = compile_stream(compiler, input_name, stream, &bc, &bc_len);

  ws_stream_close(stream);

  if (result == WS_OK)
    {
      /* Store the result to the file. */
      if (fwrite(bc, 1, bc_len, output) != bc_len)
	result = WS_ERROR_IO;

      ws_bc_data_free(bc);
    }

  return result;
}


WsResult
ws_compile_data(WsCompilerPtr compiler, const char *input_name,
		const unsigned char *input, size_t input_len,
		unsigned char **output_return, size_t *output_len_return)
{
  WsResult result;
  WsStream *stream;

  /* Initialize the input stream. */
  stream = ws_stream_new_data_input(input, input_len);
  if (stream == NULL)
    return WS_ERROR_OUT_OF_MEMORY;

  result = compile_stream(compiler, input_name, stream, output_return,
			  output_len_return);

  ws_stream_close(stream);

  return result;
}


void
ws_free_byte_code(unsigned char *byte_code)
{
  ws_bc_data_free(byte_code);
}


const char *
ws_result_to_string(WsResult result)
{
  int i;

  for (i = 0; result_codes[i].description; i++)
    if (result_codes[i].code == result)
      return result_codes[i].description;

  return "unknown result code";
}


/********************* Lexer's memory handling helpers ******************/

WsBool
ws_lexer_register_block(WsCompiler *compiler, void *ptr)
{
  void **n;

  if (ptr == NULL)
    return WS_TRUE;

  n = ws_realloc(compiler->lexer_active_list,
		 ((compiler->lexer_active_list_size + 1) * sizeof(void *)));
  if (n == NULL)
    return WS_FALSE;

  compiler->lexer_active_list = n;
  compiler->lexer_active_list[compiler->lexer_active_list_size++] = ptr;

  return WS_TRUE;
}


WsBool
ws_lexer_register_utf8(WsCompiler *compiler, WsUtf8String *string)
{
  if (!ws_lexer_register_block(compiler, string))
    return WS_FALSE;

  if (!ws_lexer_register_block(compiler, string->data))
    {
      compiler->lexer_active_list_size--;
      return WS_FALSE;
    }

  return WS_TRUE;
}


void
ws_lexer_free_block(WsCompiler *compiler, void *ptr)
{
  int i;

  if (ptr == NULL)
    return;

  for (i = compiler->lexer_active_list_size - 1; i >= 0; i--)
    if (compiler->lexer_active_list[i] == ptr)
      {
	memmove(&compiler->lexer_active_list[i],
		&compiler->lexer_active_list[i + 1],
		(compiler->lexer_active_list_size - i - 1) * sizeof(void *));
	compiler->lexer_active_list_size--;

	ws_free(ptr);
	return;
      }

  ws_fatal("ws_lexer_free_block(): unknown block 0x%lx",
	   (unsigned long) ptr);
}


void
ws_lexer_free_utf8(WsCompiler *compiler, WsUtf8String *string)
{
  if (string == NULL)
    return;

  ws_lexer_free_block(compiler, string->data);
  ws_lexer_free_block(compiler, string);
}

/********************* Static functions *********************************/

static WsResult
compile_stream(WsCompilerPtr compiler, const char *input_name,
	       WsStream *input, unsigned char **output_return,
	       size_t *output_len_return)
{
  WsResult result = WS_OK;
  WsUInt32 i;
  WsListItem *li;
  WsUInt8 findex;
  WsUInt8 num_locals;
  WsBcStringEncoding string_encoding = WS_BC_STRING_ENC_UTF8;

  /* Initialize the compiler context. */

  compiler->linenum = 1;
  compiler->input_name = input_name;

  compiler->num_errors = 0;
  compiler->num_warnings = 0;
  compiler->num_extern_functions = 0;
  compiler->num_local_functions = 0;
  compiler->errors = 0;

  /* Allocate fast-malloc pool for the syntax tree. */

  compiler->pool_stree = ws_f_create(1024 * 1024);
  if (compiler->pool_stree == NULL)
    {
      result = WS_ERROR_OUT_OF_MEMORY;
      goto out;
    }

  /* Allocate hash tables. */

  compiler->pragma_use_hash = ws_pragma_use_hash_create();
  if (compiler->pragma_use_hash == NULL)
    {
      result = WS_ERROR_OUT_OF_MEMORY;
      goto out;
    }

  compiler->functions_hash = ws_function_hash_create();
  if (compiler->functions_hash == NULL)
    {
      result = WS_ERROR_OUT_OF_MEMORY;
      goto out;
    }

  /* Allocate a byte-code module. */

  if (compiler->params.use_latin1_strings)
    string_encoding = WS_BC_STRING_ENC_ISO_8859_1;

  compiler->bc = ws_bc_alloc(string_encoding);
  if (compiler->bc == NULL)
    {
      result = WS_ERROR_OUT_OF_MEMORY;
      goto out;
    }

  /* Save the input stream. */
  compiler->input = input;

  /* Parse the input. */
#if WS_DEBUG
  global_compiler = compiler;
#endif /* WS_DEBUG */

  (void) ws_yy_parse(compiler);

  /* Free all lexer's active not freed blocks.  If we have any blocks
     on the used list, our compilation was not successful. */
  {
    size_t j;

    for (j = 0; j < compiler->lexer_active_list_size; j++)
      ws_free(compiler->lexer_active_list[j]);
    ws_free(compiler->lexer_active_list);

    compiler->lexer_active_list = NULL;
  }

  WS_CHECK_COMPILE_ERROR();

  /* XXX Sort functions. */

  /* Linearize functions */
  for (i = 0; i < compiler->num_functions; i++)
    {
      WsFunction *func = &compiler->functions[i];

      ws_info(compiler, "linearizing function `%s'...", func->name);

      compiler->pool_asm = ws_f_create(100 * 1024);
      if (compiler->pool_asm == NULL)
	{
	  result = WS_ERROR_OUT_OF_MEMORY;
	  goto out;
	}

      compiler->next_label = 0;
      compiler->asm_head = compiler->asm_tail = NULL;

      /* Create variables namespace. */
      compiler->next_vindex = 0;
      compiler->variables_hash = ws_variable_hash_create();
      if (compiler->variables_hash == NULL)
	{
	  result = WS_ERROR_OUT_OF_MEMORY;
	  goto out;
	}

      /* Define the formal arguments to the namespace. */
      for (li = func->params->head; li; li = li->next)
	{
	  WsPair *pair = (WsPair *) li->data;

	  /* The pair is `(linenumber . identifier)'.  */
	  ws_variable_define(compiler, (WsUInt32) pair->car, WS_FALSE,
			     (char *) pair->cdr);
	}

      WS_CHECK_COMPILE_ERROR();

      /* Linearize it. */
      for (li = func->block->head; li; li = li->next)
	ws_stmt_linearize(compiler, li->data);

      WS_CHECK_COMPILE_ERROR();

      /* Optimize symbolic assembler. */
      ws_asm_optimize(compiler);

      /* Print the resulting symbolic assembler if requested. */
      if (compiler->params.print_symbolic_assembler)
	ws_asm_print(compiler);

      WS_CHECK_COMPILE_ERROR();

      /* Generate byte-code */

      ws_buffer_init(&compiler->byte_code);
      ws_asm_linearize(compiler);

      WS_CHECK_COMPILE_ERROR();

      /* Disassemble the output if requested. */
      if (compiler->params.print_assembler)
	ws_asm_dasm(compiler, ws_buffer_ptr(&compiler->byte_code),
		    ws_buffer_len(&compiler->byte_code));

      /* Calculate the number of local variables */
      num_locals = compiler->next_vindex - func->params->num_items;

      /* Add the function to the byte-code module. */
      if (!ws_bc_add_function(compiler->bc, &findex,
			      func->name,
			      func->params->num_items,
			      num_locals,
			      ws_buffer_len(&compiler->byte_code),
			      ws_buffer_ptr(&compiler->byte_code)))
	{
	  result = WS_ERROR_OUT_OF_MEMORY;
	  goto out;
	}

      /* Cleanup and prepare for the next function. */

      ws_buffer_uninit(&compiler->byte_code);

      ws_hash_destroy(compiler->variables_hash);
      compiler->variables_hash = NULL;

      ws_f_destroy(compiler->pool_asm);
      compiler->pool_asm = NULL;
    }

  /* Linearize the byte-code structure. */
  if (!ws_bc_encode(compiler->bc, output_return, output_len_return))
    result = WS_ERROR_OUT_OF_MEMORY;

 out:

  /* Cleanup. */

  ws_f_destroy(compiler->pool_stree);
  compiler->pool_stree = NULL;

  ws_hash_destroy(compiler->pragma_use_hash);
  compiler->pragma_use_hash = NULL;

  /* Free functions. */
  for (i = 0; i < compiler->num_functions; i++)
    ws_free(compiler->functions[i].name);
  ws_free(compiler->functions);

  ws_hash_destroy(compiler->functions_hash);
  compiler->functions_hash = NULL;

  ws_bc_free(compiler->bc);
  compiler->bc = NULL;

  compiler->input = NULL;

  ws_f_destroy(compiler->pool_asm);
  compiler->pool_asm = NULL;

  ws_hash_destroy(compiler->variables_hash);
  compiler->variables_hash = NULL;

  ws_buffer_uninit(&compiler->byte_code);

  /* All done. */
  return result;
}


static void
std_io(const char *data, size_t len, void *context)
{
  fwrite(data, 1, len, (FILE *) context);
}
