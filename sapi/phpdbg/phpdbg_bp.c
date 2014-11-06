/*
   +----------------------------------------------------------------------+
   | PHP Version 5                                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 1997-2014 The PHP Group                                |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Authors: Felipe Pena <felipe@php.net>                                |
   | Authors: Joe Watkins <joe.watkins@live.co.uk>                        |
   | Authors: Bob Weinand <bwoebi@php.net>                                |
   +----------------------------------------------------------------------+
*/

#include "zend.h"
#include "zend_hash.h"
#include "phpdbg.h"
#include "phpdbg_bp.h"
#include "phpdbg_utils.h"
#include "phpdbg_opcode.h"
#include "zend_globals.h"

ZEND_EXTERN_MODULE_GLOBALS(phpdbg);

/* {{{ private api functions */
static inline phpdbg_breakbase_t *phpdbg_find_breakpoint_file(zend_op_array* TSRMLS_DC);
static inline phpdbg_breakbase_t *phpdbg_find_breakpoint_symbol(zend_function* TSRMLS_DC);
static inline phpdbg_breakbase_t *phpdbg_find_breakpoint_method(zend_op_array* TSRMLS_DC);
static inline phpdbg_breakbase_t *phpdbg_find_breakpoint_opline(phpdbg_opline_ptr_t TSRMLS_DC);
static inline phpdbg_breakbase_t *phpdbg_find_breakpoint_opcode(zend_uchar TSRMLS_DC);
static inline phpdbg_breakbase_t *phpdbg_find_conditional_breakpoint(zend_execute_data *execute_data TSRMLS_DC); /* }}} */

/*
* Note:
*	A break point must always set the correct id and type
*	A set breakpoint function must always map new points
*/
static inline void _phpdbg_break_mapping(int id, HashTable *table TSRMLS_DC)
{
	zend_hash_index_update(&PHPDBG_G(bp)[PHPDBG_BREAK_MAP], (id), (void**) &table, sizeof(void*), NULL);
}

#define PHPDBG_BREAK_MAPPING(id, table) _phpdbg_break_mapping(id, table TSRMLS_CC)
#define PHPDBG_BREAK_UNMAPPING(id) \
	zend_hash_index_del(&PHPDBG_G(bp)[PHPDBG_BREAK_MAP], (id))

#define PHPDBG_BREAK_INIT(b, t) do {\
	b.id = PHPDBG_G(bp_count)++; \
	b.type = t; \
	b.disabled = 0;\
	b.hits = 0; \
} while(0)

static void phpdbg_file_breaks_dtor(void *data) /* {{{ */
{
	phpdbg_breakfile_t *bp = (phpdbg_breakfile_t*) data;

	efree((char*)bp->filename);
} /* }}} */

static void phpdbg_class_breaks_dtor(void *data) /* {{{ */
{
	phpdbg_breakmethod_t *bp = (phpdbg_breakmethod_t*) data;

	efree((char*)bp->class_name);
	efree((char*)bp->func_name);
} /* }}} */

static void phpdbg_opline_class_breaks_dtor(void *data) /* {{{ */
{
	zend_hash_destroy((HashTable *)data);
} /* }}} */

static void phpdbg_opline_breaks_dtor(void *data) /* {{{ */
{
	phpdbg_breakopline_t *bp = (phpdbg_breakopline_t *) data;

	if (bp->class_name) {
		efree((char*)bp->class_name);
	}
	if (bp->func_name) {
		efree((char*)bp->func_name);
	}
} /* }}} */

PHPDBG_API void phpdbg_reset_breakpoints(TSRMLS_D) /* {{{ */
{
	if (zend_hash_num_elements(&PHPDBG_G(bp)[PHPDBG_BREAK_MAP])) {
		HashPosition position[2];
		HashTable **table = NULL;

		for (zend_hash_internal_pointer_reset_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_MAP], &position[0]);
		     zend_hash_get_current_data_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_MAP], (void **) &table, &position[0]) == SUCCESS;
		     zend_hash_move_forward_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_MAP], &position[0])) {
			phpdbg_breakbase_t *brake;

			for (zend_hash_internal_pointer_reset_ex((*table), &position[1]);
				zend_hash_get_current_data_ex((*table), (void**)&brake, &position[1]) == SUCCESS;
				zend_hash_move_forward_ex((*table), &position[1])) {
				brake->hits = 0;
			}
		}
	}
} /* }}} */

PHPDBG_API void phpdbg_export_breakpoints(FILE *handle TSRMLS_DC) /* {{{ */
{
	char *string;
	phpdbg_export_breakpoints_to_string(&string TSRMLS_CC);
	fputs(string, handle);
}

PHPDBG_API void phpdbg_export_breakpoints_to_string(char **str TSRMLS_DC) /* {{{ */
{
	HashPosition position[2];
	HashTable **table = NULL;
	zend_ulong id = 0L;

	*str = "";

	if (zend_hash_num_elements(&PHPDBG_G(bp)[PHPDBG_BREAK_MAP])) {
		phpdbg_notice("exportbreakpoint", "count=\"%d\"", "Exporting %d breakpoints", zend_hash_num_elements(&PHPDBG_G(bp)[PHPDBG_BREAK_MAP]));
		/* this only looks like magic, it isn't */
		for (zend_hash_internal_pointer_reset_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_MAP], &position[0]);
		     zend_hash_get_current_data_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_MAP], (void **) &table, &position[0]) == SUCCESS;
		     zend_hash_move_forward_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_MAP], &position[0])) {
			phpdbg_breakbase_t *brake;

			zend_hash_get_current_key_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_MAP], NULL, NULL, &id, 0, &position[0]);

			for (zend_hash_internal_pointer_reset_ex((*table), &position[1]);
			     zend_hash_get_current_data_ex((*table), (void**)&brake, &position[1]) == SUCCESS;
			     zend_hash_move_forward_ex((*table), &position[1])) {
				if (brake->id == id) {
					char *new_str = NULL;

					switch (brake->type) {
						case PHPDBG_BREAK_FILE: {
							phpdbg_asprintf(&new_str,
								"%sbreak %s:%lu\n", *str,
								((phpdbg_breakfile_t*)brake)->filename,
								((phpdbg_breakfile_t*)brake)->line);
						} break;

						case PHPDBG_BREAK_SYM: {
							phpdbg_asprintf(&new_str,
								"%sbreak %s\n", *str,
								((phpdbg_breaksymbol_t*)brake)->symbol);
						} break;

						case PHPDBG_BREAK_METHOD: {
							phpdbg_asprintf(&new_str,
								"%sbreak %s::%s\n", *str,
								((phpdbg_breakmethod_t*)brake)->class_name,
								((phpdbg_breakmethod_t*)brake)->func_name);
						} break;

						case PHPDBG_BREAK_METHOD_OPLINE: {
							phpdbg_asprintf(&new_str,
								"%sbreak %s::%s#%ld\n", *str,
								((phpdbg_breakopline_t*)brake)->class_name,
								((phpdbg_breakopline_t*)brake)->func_name,
								((phpdbg_breakopline_t*)brake)->opline_num);
						} break;

						case PHPDBG_BREAK_FUNCTION_OPLINE: {
							phpdbg_asprintf(&new_str,
								"%sbreak %s#%ld\n", *str,
								((phpdbg_breakopline_t*)brake)->func_name,
								((phpdbg_breakopline_t*)brake)->opline_num);
						} break;

						case PHPDBG_BREAK_FILE_OPLINE: {
							phpdbg_asprintf(&new_str,
								"%sbreak %s:#%ld\n", *str,
								((phpdbg_breakopline_t*)brake)->class_name,
								((phpdbg_breakopline_t*)brake)->opline_num);
						} break;

						case PHPDBG_BREAK_OPCODE: {
							phpdbg_asprintf(&new_str,
								"%sbreak %s\n", *str,
								((phpdbg_breakop_t*)brake)->name);
						} break;

						case PHPDBG_BREAK_COND: {
							phpdbg_breakcond_t *conditional = (phpdbg_breakcond_t*) brake;

							if (conditional->paramed) {
								switch (conditional->param.type) {
									case STR_PARAM:
										phpdbg_asprintf(&new_str,
											"%sbreak at %s if %s\n", *str, conditional->param.str, conditional->code);
									break;

									case METHOD_PARAM:
										phpdbg_asprintf(&new_str,
											"%sbreak at %s::%s if %s\n", *str,
											conditional->param.method.class, conditional->param.method.name,
											conditional->code);
									break;

									case FILE_PARAM:
										phpdbg_asprintf(&new_str,
											"%sbreak at %s:%lu if %s\n", *str,
											conditional->param.file.name, conditional->param.file.line,
											conditional->code);
									break;

									default: { /* do nothing */ } break;
								}
							} else {
								phpdbg_asprintf(&new_str, "%sbreak if %s\n", str, conditional->code);
							}
						} break;
					}

					if ((*str)[0]) {
						efree(*str);
					}
					*str = new_str;
				}
			}
		}
	}

	if (!(*str)[0]) {
		*str = NULL;
	}
} /* }}} */

PHPDBG_API void phpdbg_set_breakpoint_file(const char *path, long line_num TSRMLS_DC) /* {{{ */
{
	php_stream_statbuf ssb;
	char realpath[MAXPATHLEN];
	const char *original_path = path;
	zend_bool pending = 0;

	HashTable *broken, *file_breaks =  &PHPDBG_G(bp)[PHPDBG_BREAK_FILE];
	phpdbg_breakfile_t new_break;
	size_t path_len = 0L;

	if (VCWD_REALPATH(path, realpath)) {
		path = realpath;
	}
	path_len = strlen(path);

	phpdbg_debug("file path: %s, resolved path: %s, was compiled: %d\n", original_path, path, zend_hash_exists(&PHPDBG_G(file_sources), path, path_len));

	if (!zend_hash_exists(&PHPDBG_G(file_sources), path, path_len)) {
		if (php_stream_stat_path(path, &ssb) == FAILURE) {
			if (original_path[0] == '/') {
				phpdbg_error("breakpoint", "type=\"nofile\" add=\"fail\" file=\"%s\"", "Cannot stat %s, it does not exist", original_path);
				return;
			}

			file_breaks = &PHPDBG_G(bp)[PHPDBG_BREAK_FILE_PENDING];
			path = original_path;
			path_len = strlen(path);
			pending = 1;
		} else if (!(ssb.sb.st_mode & (S_IFREG|S_IFLNK))) {
			phpdbg_error("breakpoint", "type=\"notregular\" add=\"fail\" file=\"%s\"", "Cannot set breakpoint in %s, it is not a regular file", path);
			return;
		} else {
			phpdbg_debug("File exists, but not compiled\n");
		}
	}

	if (zend_hash_find(file_breaks, path, path_len, (void **) &broken) == FAILURE) {
		HashTable breaks;
		zend_hash_init(&breaks, 8, NULL, phpdbg_file_breaks_dtor, 0);

		zend_hash_add(file_breaks, path, path_len, &breaks, sizeof(HashTable), (void **) &broken);
	}

	if (!zend_hash_index_exists(broken, line_num)) {
		HashPosition position;
		char *file;
		uint filelen;

		PHPDBG_BREAK_INIT(new_break, PHPDBG_BREAK_FILE);
		new_break.filename = estrndup(path, path_len);
		new_break.line = line_num;

		zend_hash_index_update(broken, line_num, (void **) &new_break, sizeof(phpdbg_breakfile_t), NULL);

		PHPDBG_BREAK_MAPPING(new_break.id, broken);

		if (pending) {
			for (zend_hash_internal_pointer_reset_ex(&PHPDBG_G(file_sources), &position);
			     zend_hash_get_current_key_ex(&PHPDBG_G(file_sources), &file, &filelen, NULL, 0, &position) == HASH_KEY_IS_STRING;
			     zend_hash_move_forward_ex(&PHPDBG_G(file_sources), &position)) {
				HashTable *fileht;

				phpdbg_debug("Compare against loaded %s\n", file);

				if (!(pending = ((fileht = phpdbg_resolve_pending_file_break_ex(file, filelen, path, path_len, broken TSRMLS_CC)) == NULL))) {
					phpdbg_breakfile_t *brake;
					zend_hash_index_find(fileht, line_num, (void **) &brake);
					new_break = *brake;
					break;
				}
			}
		}

		if (pending) {
			PHPDBG_G(flags) |= PHPDBG_HAS_PENDING_FILE_BP;

			phpdbg_notice("breakpoint", "add=\"success\" id=\"%d\" file=\"%s\" line=\"%ld\" pending=\"pending\"", "Pending breakpoint #%d added at %s:%ld", new_break.id, new_break.filename, new_break.line);
		} else {
			PHPDBG_G(flags) |= PHPDBG_HAS_FILE_BP;

			phpdbg_notice("breakpoint", "add=\"success\" id=\"%d\" file=\"%s\" line=\"%ld\"", "Breakpoint #%d added at %s:%ld", new_break.id, new_break.filename, new_break.line);
		}
	} else {
		phpdbg_error("breakpoint", "type=\"exists\" add=\"fail\" file=\"%s\" line=\"%ld\"", "Breakpoint at %s:%ld exists", path, line_num);
	}
} /* }}} */

PHPDBG_API HashTable *phpdbg_resolve_pending_file_break_ex(const char *file, uint filelen, const char *cur, uint curlen, HashTable *fileht TSRMLS_DC) /* {{{ */
{
	phpdbg_debug("file: %s, filelen: %u, cur: %s, curlen %u, pos: %c, memcmp: %d\n", file, filelen, cur, curlen, filelen > curlen ? file[filelen - curlen - 1] : '?', filelen > curlen ? memcmp(file + filelen - curlen, cur, curlen) : 0);

	if (((curlen < filelen && file[filelen - curlen - 1] == '/') || filelen == curlen) && !memcmp(file + filelen - curlen, cur, curlen)) {
		phpdbg_breakfile_t *brake, new_brake;
		HashTable *master = NULL;
		HashPosition position;

		PHPDBG_G(flags) |= PHPDBG_HAS_FILE_BP;

		if (zend_hash_find(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE], file, filelen, (void **) &master) == FAILURE) {
			HashTable new_ht;
			zend_hash_init(&new_ht, 8, NULL, phpdbg_file_breaks_dtor, 0);
			zend_hash_add(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE], file, filelen, &new_ht, sizeof(HashTable), (void **) &master);
		}

		for (zend_hash_internal_pointer_reset_ex(fileht, &position);
		     zend_hash_get_current_data_ex(fileht, (void**)&brake, &position) == SUCCESS;
		     zend_hash_move_forward_ex(fileht, &position)) {
			new_brake = *brake;
			new_brake.filename = estrndup(file, filelen);
			PHPDBG_BREAK_UNMAPPING(brake->id);

			if (master) {
				zend_hash_index_update(master, brake->line, (void **) &new_brake, sizeof(phpdbg_breakfile_t), NULL);
				PHPDBG_BREAK_MAPPING(brake->id, master);
			}
		}

		zend_hash_del(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_PENDING], cur, curlen);

		if (!zend_hash_num_elements(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_PENDING])) {
			PHPDBG_G(flags) &= ~PHPDBG_HAS_PENDING_FILE_BP;
		}

		phpdbg_debug("compiled file: %s, cur bp file: %s\n", file, cur);

		return master;
	}

	return NULL;
} /* }}} */

PHPDBG_API void phpdbg_resolve_pending_file_break(const char *file TSRMLS_DC) /* {{{ */
{
	HashPosition position;
	HashTable *fileht;
	uint filelen = strlen(file);

	phpdbg_debug("was compiled: %s\n", file);

	zend_hash_internal_pointer_reset_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_PENDING], &position);
	while (zend_hash_get_current_data_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_PENDING], (void**) &fileht, &position) == SUCCESS) {
		const char *cur;
		uint curlen;

		zend_hash_get_current_key_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_PENDING], (char **) &cur, &curlen, NULL, 0, &position);
		zend_hash_move_forward_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_PENDING], &position);

		phpdbg_debug("check bp: %s\n", cur);

		phpdbg_resolve_pending_file_break_ex(file, filelen, cur, curlen, fileht TSRMLS_CC);
	}
}

PHPDBG_API void phpdbg_set_breakpoint_symbol(const char *name, size_t name_len TSRMLS_DC) /* {{{ */
{
	if (!zend_hash_exists(&PHPDBG_G(bp)[PHPDBG_BREAK_SYM], name, name_len)) {
		phpdbg_breaksymbol_t new_break;

		PHPDBG_G(flags) |= PHPDBG_HAS_SYM_BP;

		PHPDBG_BREAK_INIT(new_break, PHPDBG_BREAK_SYM);
		new_break.symbol = estrndup(name, name_len);

		zend_hash_update(&PHPDBG_G(bp)[PHPDBG_BREAK_SYM], new_break.symbol,
			name_len, &new_break, sizeof(phpdbg_breaksymbol_t), NULL);

		phpdbg_notice("breakpoint", "add=\"success\" id=\"%d\" function=\"%s\"", "Breakpoint #%d added at %s",
			new_break.id, new_break.symbol);

		PHPDBG_BREAK_MAPPING(new_break.id, &PHPDBG_G(bp)[PHPDBG_BREAK_SYM]);
	} else {
		phpdbg_error("breakpoint", "type=\"exists\" add=\"fail\" function=\"%s\"", "Breakpoint exists at %s", name);
	}
} /* }}} */

PHPDBG_API void phpdbg_set_breakpoint_method(const char *class_name, const char *func_name TSRMLS_DC) /* {{{ */
{
	HashTable class_breaks, *class_table;
	size_t class_len = strlen(class_name);
	size_t func_len = strlen(func_name);
	char *lcname = zend_str_tolower_dup(func_name, func_len);

	if (zend_hash_find(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD], class_name,
		class_len, (void**)&class_table) != SUCCESS) {
		zend_hash_init(&class_breaks, 8, NULL, phpdbg_class_breaks_dtor, 0);
		zend_hash_update(
			&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD],
			class_name, class_len,
			(void**)&class_breaks, sizeof(HashTable), (void**)&class_table);
	}

	if (!zend_hash_exists(class_table, lcname, func_len)) {
		phpdbg_breakmethod_t new_break;

		PHPDBG_G(flags) |= PHPDBG_HAS_METHOD_BP;

		PHPDBG_BREAK_INIT(new_break, PHPDBG_BREAK_METHOD);
		new_break.class_name = estrndup(class_name, class_len);
		new_break.class_len = class_len;
		new_break.func_name = estrndup(func_name, func_len);
		new_break.func_len = func_len;

		zend_hash_update(class_table, lcname, func_len,
			&new_break, sizeof(phpdbg_breakmethod_t), NULL);

		phpdbg_notice("breakpoint", "add=\"success\" id=\"%d\" method=\"%s::%s\"", "Breakpoint #%d added at %s::%s",
			new_break.id, class_name, func_name);

		PHPDBG_BREAK_MAPPING(new_break.id, class_table);
	} else {
		phpdbg_error("breakpoint", "type=\"exists\" add=\"fail\" method=\"%s::%s\"", "Breakpoint exists at %s::%s", class_name, func_name);
	}

	efree(lcname);
} /* }}} */

PHPDBG_API void phpdbg_set_breakpoint_opline(zend_ulong opline TSRMLS_DC) /* {{{ */
{
	if (!zend_hash_index_exists(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE], opline)) {
		phpdbg_breakline_t new_break;

		PHPDBG_G(flags) |= PHPDBG_HAS_OPLINE_BP;

		PHPDBG_BREAK_INIT(new_break, PHPDBG_BREAK_OPLINE);
		new_break.name = NULL;
		new_break.opline = opline;
		new_break.base = NULL;

		zend_hash_index_update(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE], opline,
			&new_break, sizeof(phpdbg_breakline_t), NULL);

		phpdbg_notice("breakpoint", "add=\"success\" id=\"%d\" opline=\"%#lx\"", "Breakpoint #%d added at %#lx",
			new_break.id, new_break.opline);
		PHPDBG_BREAK_MAPPING(new_break.id, &PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE]);
	} else {
		phpdbg_error("breakpoint", "type=\"exists\" add=\"fail\" opline=\"%#lx\"", "Breakpoint exists at %#lx", opline);
	}
} /* }}} */

PHPDBG_API int phpdbg_resolve_op_array_break(phpdbg_breakopline_t *brake, zend_op_array *op_array TSRMLS_DC) /* {{{ */
{
	phpdbg_breakline_t opline_break;
	if (op_array->last <= brake->opline_num) {
		if (brake->class_name == NULL) {
			phpdbg_error("breakpoint", "type=\"maxoplines\" add=\"fail\" maxoplinenum=\"%d\" function=\"%s\" usedoplinenum=\"%ld\"", "There are only %d oplines in function %s (breaking at opline %ld impossible)", op_array->last, brake->func_name, brake->opline_num);
		} else if (brake->func_name == NULL) {
			phpdbg_error("breakpoint", "type=\"maxoplines\" add=\"fail\" maxoplinenum=\"%d\" file=\"%s\" usedoplinenum=\"%ld\"", "There are only %d oplines in file %s (breaking at opline %ld impossible)", op_array->last, brake->class_name, brake->opline_num);
		} else {
			phpdbg_error("breakpoint", "type=\"maxoplines\" add=\"fail\" maxoplinenum=\"%d\" method=\"%s::%s\" usedoplinenum=\"%ld\"", "There are only %d oplines in method %s::%s (breaking at opline %ld impossible)", op_array->last, brake->class_name, brake->func_name, brake->opline_num);
		}

		return FAILURE;
	}

	opline_break.disabled = 0;
	opline_break.hits = 0;
	opline_break.id = brake->id;
	opline_break.opline = brake->opline = (zend_ulong)(op_array->opcodes + brake->opline_num);
	opline_break.name = NULL;
	opline_break.base = brake;
	if (op_array->scope) {
		opline_break.type = PHPDBG_BREAK_METHOD_OPLINE;
	} else if (op_array->function_name) {
		opline_break.type = PHPDBG_BREAK_FUNCTION_OPLINE;
	} else {
		opline_break.type = PHPDBG_BREAK_FILE_OPLINE;
	}

	PHPDBG_G(flags) |= PHPDBG_HAS_OPLINE_BP;

	zend_hash_index_update(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE], opline_break.opline, &opline_break, sizeof(phpdbg_breakline_t), NULL);

	return SUCCESS;
} /* }}} */

PHPDBG_API void phpdbg_resolve_op_array_breaks(zend_op_array *op_array TSRMLS_DC) /* {{{ */
{
	HashTable *func_table = &PHPDBG_G(bp)[PHPDBG_BREAK_FUNCTION_OPLINE];
	HashTable *oplines_table;
	HashPosition position;
	phpdbg_breakopline_t *brake;

	if (op_array->scope != NULL &&
	    zend_hash_find(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD_OPLINE], op_array->scope->name, op_array->scope->name_length, (void **)&func_table) == FAILURE) {
		return;
	}

	if (op_array->function_name == NULL) {
		if (zend_hash_find(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_OPLINE], op_array->filename, strlen(op_array->filename), (void **)&oplines_table) == FAILURE) {
			return;
		}
	} else if (zend_hash_find(func_table, op_array->function_name?op_array->function_name:"", op_array->function_name?strlen(op_array->function_name):0, (void **)&oplines_table) == FAILURE) {
		return;
	}

	for (zend_hash_internal_pointer_reset_ex(oplines_table, &position);
	     zend_hash_get_current_data_ex(oplines_table, (void**) &brake, &position) == SUCCESS;
	     zend_hash_move_forward_ex(oplines_table, &position)) {
		if (phpdbg_resolve_op_array_break(brake, op_array TSRMLS_CC) == SUCCESS) {
			phpdbg_breakline_t *opline_break;

			zend_hash_internal_pointer_end(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE]);
			zend_hash_get_current_data(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE], (void **)&opline_break);

			phpdbg_notice("breakpoint", "add=\"success\" id=\"%d\" symbol=\"%s\" num=\"%ld\" opline=\"%#lx\"", "Breakpoint #%d resolved at %s%s%s#%ld (opline %#lx)",
				brake->id,
				brake->class_name?brake->class_name:"",
				brake->class_name&&brake->func_name?"::":"",
				brake->func_name?brake->func_name:"",
				brake->opline_num,
				brake->opline);
		}
	}
} /* }}} */

PHPDBG_API int phpdbg_resolve_opline_break(phpdbg_breakopline_t *new_break TSRMLS_DC) /* {{{ */
{
	HashTable *func_table = EG(function_table);
	zend_function *func;

	if (new_break->func_name == NULL) {
		if (EG(current_execute_data) == NULL) {
			if (PHPDBG_G(ops) != NULL && !memcmp(PHPDBG_G(ops)->filename, new_break->class_name, new_break->class_len)) {
				if (phpdbg_resolve_op_array_break(new_break, PHPDBG_G(ops) TSRMLS_CC) == SUCCESS) {
					return SUCCESS;
				} else {
					return 2;
				}
			}
			return FAILURE;
		} else {
			zend_execute_data *execute_data = EG(current_execute_data);
			do {
				if (execute_data->op_array->function_name == NULL && execute_data->op_array->scope == NULL && !memcmp(execute_data->op_array->filename, new_break->class_name, new_break->class_len)) {
					if (phpdbg_resolve_op_array_break(new_break, execute_data->op_array TSRMLS_CC) == SUCCESS) {
						return SUCCESS;
					} else {
						return 2;
					}
				}
			} while ((execute_data = execute_data->prev_execute_data) != NULL);
			return FAILURE;
		}
	}

	if (new_break->class_name != NULL) {
		zend_class_entry **ce;
		if (zend_hash_find(EG(class_table), zend_str_tolower_dup(new_break->class_name, new_break->class_len), new_break->class_len + 1, (void **)&ce) == FAILURE) {
			return FAILURE;
		}
		func_table = &(*ce)->function_table;
	}

	if (zend_hash_find(func_table, zend_str_tolower_dup(new_break->func_name, new_break->func_len), new_break->func_len + 1, (void **)&func) == FAILURE) {
		if (new_break->class_name != NULL && new_break->func_name != NULL) {
			phpdbg_error("breakpoint", "type=\"nomethod\" method=\"%s::%s\"", "Method %s doesn't exist in class %s", new_break->func_name, new_break->class_name);
			return 2;
		}
		return FAILURE;
	}

	if (func->type != ZEND_USER_FUNCTION) {
		if (new_break->class_name == NULL) {
			phpdbg_error("breakpoint", "type=\"internalfunction\" function=\"%s\"", "%s is not an user defined function, no oplines exist", new_break->func_name);
		} else {
			phpdbg_error("breakpoint", "type=\"internalfunction\" method=\"%s::%s\"", "%s::%s is not an user defined method, no oplines exist", new_break->class_name, new_break->func_name);
		}
		return 2;
	}

	if (phpdbg_resolve_op_array_break(new_break, &func->op_array TSRMLS_CC) == FAILURE) {
		return 2;
	}

	return SUCCESS;
} /* }}} */

PHPDBG_API void phpdbg_set_breakpoint_method_opline(const char *class, const char *method, zend_ulong opline TSRMLS_DC) /* {{{ */
{
	phpdbg_breakopline_t new_break;
	HashTable class_breaks, *class_table;
	HashTable method_breaks, *method_table;

	PHPDBG_BREAK_INIT(new_break, PHPDBG_BREAK_METHOD_OPLINE);
	new_break.func_len = strlen(method);
	new_break.func_name = estrndup(method, new_break.func_len);
	new_break.class_len = strlen(class);
	new_break.class_name = estrndup(class, new_break.class_len);
	new_break.opline_num = opline;
	new_break.opline = 0;

	switch (phpdbg_resolve_opline_break(&new_break TSRMLS_CC)) {
		case FAILURE:
			phpdbg_notice("breakpoint", "pending=\"pending\" id=\"%d\" method=\"%::%s\" num=\"%ld\"", "Pending breakpoint #%d at %s::%s#%ld", new_break.id, new_break.class_name, new_break.func_name, opline);
			break;

		case SUCCESS:
			phpdbg_notice("breakpoint", "id=\"%d\" method=\"%::%s\" num=\"%ld\"", "Breakpoint #%d added at %s::%s#%ld", new_break.id, new_break.class_name, new_break.func_name, opline);
			break;

		case 2:
			return;
	}

	if (zend_hash_find(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD_OPLINE], new_break.class_name, new_break.class_len, (void **)&class_table) == FAILURE) {
		zend_hash_init(&class_breaks, 8, NULL, phpdbg_opline_class_breaks_dtor, 0);
		zend_hash_update(
			&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD_OPLINE],
			new_break.class_name,
			new_break.class_len,
			(void **)&class_breaks, sizeof(HashTable), (void **)&class_table);
	}

	if (zend_hash_find(class_table, new_break.func_name, new_break.func_len, (void **)&method_table) == FAILURE) {
		zend_hash_init(&method_breaks, 8, NULL, phpdbg_opline_breaks_dtor, 0);
		zend_hash_update(
			class_table,
			new_break.func_name,
			new_break.func_len,
			(void **)&method_breaks, sizeof(HashTable), (void **)&method_table);
	}

	if (zend_hash_index_exists(method_table, opline)) {
		phpdbg_error("breakpoint", "type=\"exists\" method=\"%s\" num=\"%ld\"", "Breakpoint already exists for %s::%s#%ld", new_break.class_name, new_break.func_name, opline);
		efree((char*)new_break.func_name);
		efree((char*)new_break.class_name);
		PHPDBG_G(bp_count)--;
		return;
	}

	PHPDBG_G(flags) |= PHPDBG_HAS_METHOD_OPLINE_BP;

	PHPDBG_BREAK_MAPPING(new_break.id, method_table);

	zend_hash_index_update(method_table, opline, &new_break, sizeof(phpdbg_breakopline_t), NULL);
}

PHPDBG_API void phpdbg_set_breakpoint_function_opline(const char *function, zend_ulong opline TSRMLS_DC) /* {{{ */
{
	phpdbg_breakopline_t new_break;
	HashTable func_breaks, *func_table;

	PHPDBG_BREAK_INIT(new_break, PHPDBG_BREAK_FUNCTION_OPLINE);
	new_break.func_len = strlen(function);
	new_break.func_name = estrndup(function, new_break.func_len);
	new_break.class_len = 0;
	new_break.class_name = NULL;
	new_break.opline_num = opline;
	new_break.opline = 0;

	switch (phpdbg_resolve_opline_break(&new_break TSRMLS_CC)) {
		case FAILURE:
			phpdbg_notice("breakpoint", "pending=\"pending\" id=\"%d\" function=\"%s\" num=\"%ld\"", "Pending breakpoint #%d at %s#%ld", new_break.id, new_break.func_name, opline);
			break;

		case SUCCESS:
			phpdbg_notice("breakpoint", "id=\"%d\" function=\"%s\" num=\"%ld\"", "Breakpoint #%d added at %s#%ld", new_break.id, new_break.func_name, opline);
			break;

		case 2:
			return;
	}

	if (zend_hash_find(&PHPDBG_G(bp)[PHPDBG_BREAK_FUNCTION_OPLINE], new_break.func_name, new_break.func_len, (void **)&func_table) == FAILURE) {
		zend_hash_init(&func_breaks, 8, NULL, phpdbg_opline_breaks_dtor, 0);
		zend_hash_update(
			&PHPDBG_G(bp)[PHPDBG_BREAK_FUNCTION_OPLINE],
			new_break.func_name,
			new_break.func_len,
			(void **)&func_breaks, sizeof(HashTable), (void **)&func_table);
	}

	if (zend_hash_index_exists(func_table, opline)) {
		phpdbg_error("breakpoint", "type=\"exists\" function=\"%s\" num=\"%ld\"", "Breakpoint already exists for %s#%ld", new_break.func_name, opline);
		efree((char*)new_break.func_name);
		PHPDBG_G(bp_count)--;
		return;
	}

	PHPDBG_BREAK_MAPPING(new_break.id, func_table);

	PHPDBG_G(flags) |= PHPDBG_HAS_FUNCTION_OPLINE_BP;

	zend_hash_index_update(func_table, opline, &new_break, sizeof(phpdbg_breakopline_t), NULL);
}

PHPDBG_API void phpdbg_set_breakpoint_file_opline(const char *file, zend_ulong opline TSRMLS_DC) /* {{{ */
{
	phpdbg_breakopline_t new_break;
	HashTable file_breaks, *file_table;

	PHPDBG_BREAK_INIT(new_break, PHPDBG_BREAK_FILE_OPLINE);
	new_break.func_len = 0;
	new_break.func_name = NULL;
	new_break.class_len = strlen(file);
	new_break.class_name = estrndup(file, new_break.class_len);
	new_break.opline_num = opline;
	new_break.opline = 0;

	switch (phpdbg_resolve_opline_break(&new_break TSRMLS_CC)) {
		case FAILURE:
			phpdbg_notice("breakpoint", "pending=\"pending\" id=\"%d\" file=\"%s\" num=\"%ld\"", "Pending breakpoint #%d at %s:%ld", new_break.id, new_break.class_name, opline);
			break;

		case SUCCESS:
			phpdbg_notice("breakpoint", "id=\"%d\" file=\"%s\" num=\"%ld\"", "Breakpoint #%d added at %s:%ld", new_break.id, new_break.class_name, opline);
			break;

		case 2:
			return;
	}

	if (zend_hash_find(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_OPLINE], new_break.class_name, new_break.class_len, (void **)&file_table) == FAILURE) {
		zend_hash_init(&file_breaks, 8, NULL, phpdbg_opline_breaks_dtor, 0);
		zend_hash_update(
			&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_OPLINE],
			new_break.class_name,
			new_break.class_len,
			(void **)&file_breaks, sizeof(HashTable), (void **)&file_table);
	}

	if (zend_hash_index_exists(file_table, opline)) {
		phpdbg_error("breakpoint", "type=\"exists\" file=\"%s\" num=\"%d\"", "Breakpoint already exists for %s:%ld", new_break.class_name, opline);
		efree((char*)new_break.class_name);
		PHPDBG_G(bp_count)--;
		return;
	}

	PHPDBG_BREAK_MAPPING(new_break.id, file_table);

	PHPDBG_G(flags) |= PHPDBG_HAS_FILE_OPLINE_BP;

	zend_hash_index_update(file_table, opline, &new_break, sizeof(phpdbg_breakopline_t), NULL);
}

PHPDBG_API void phpdbg_set_breakpoint_opcode(const char *name, size_t name_len TSRMLS_DC) /* {{{ */
{
	phpdbg_breakop_t new_break;
	zend_ulong hash = zend_hash_func(name, name_len);

	if (zend_hash_index_exists(&PHPDBG_G(bp)[PHPDBG_BREAK_OPCODE], hash)) {
		phpdbg_error("breakpoint", "type=\"exists\" opcode=\"%s\"", "Breakpoint exists for %s", name);
		return;
	}

	PHPDBG_BREAK_INIT(new_break, PHPDBG_BREAK_OPCODE);
	new_break.hash = hash;
	new_break.name = estrndup(name, name_len);

	zend_hash_index_update(&PHPDBG_G(bp)[PHPDBG_BREAK_OPCODE], hash,
		&new_break, sizeof(phpdbg_breakop_t), NULL);

	PHPDBG_G(flags) |= PHPDBG_HAS_OPCODE_BP;

	phpdbg_notice("breakpoint", "id=\"%d\" opcode=\"%s\"", "Breakpoint #%d added at %s", new_break.id, name);
	PHPDBG_BREAK_MAPPING(new_break.id, &PHPDBG_G(bp)[PHPDBG_BREAK_OPCODE]);
} /* }}} */

PHPDBG_API void phpdbg_set_breakpoint_opline_ex(phpdbg_opline_ptr_t opline TSRMLS_DC) /* {{{ */
{
	if (!zend_hash_index_exists(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE], (zend_ulong) opline)) {
		phpdbg_breakline_t new_break;

		PHPDBG_G(flags) |= PHPDBG_HAS_OPLINE_BP;

		PHPDBG_BREAK_INIT(new_break, PHPDBG_BREAK_OPLINE);
		new_break.opline = (zend_ulong) opline;

		zend_hash_index_update(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE],
			(zend_ulong) opline, &new_break, sizeof(phpdbg_breakline_t), NULL);

		phpdbg_notice("breakpoint", "id=\"%d\" opline=\"%#lx\"", "Breakpoint #%d added at %#lx", new_break.id, new_break.opline);
		PHPDBG_BREAK_MAPPING(new_break.id, &PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE]);
	} else {
		phpdbg_error("breakpoint", "type=\"exists\" opline=\"%#lx\"", "Breakpoint exists for opline %#lx", (zend_ulong) opline);
	}
} /* }}} */

static inline void phpdbg_create_conditional_break(phpdbg_breakcond_t *brake, const phpdbg_param_t *param, const char *expr, size_t expr_len, zend_ulong hash TSRMLS_DC) /* {{{ */
{
	phpdbg_breakcond_t new_break;
	zend_uint cops = CG(compiler_options);
	zval pv;

	PHPDBG_BREAK_INIT(new_break, PHPDBG_BREAK_COND);
	new_break.hash = hash;

	if (param) {
		new_break.paramed = 1;
		phpdbg_copy_param(
			param, &new_break.param TSRMLS_CC);
	} else {
		new_break.paramed = 0;
	}

	cops = CG(compiler_options);

	CG(compiler_options) = ZEND_COMPILE_DEFAULT_FOR_EVAL;

	new_break.code = estrndup(expr, expr_len);
	new_break.code_len = expr_len;

	Z_STRLEN(pv) = expr_len + sizeof("return ;") - 1;
	Z_STRVAL(pv) = emalloc(Z_STRLEN(pv) + 1);
	memcpy(Z_STRVAL(pv), "return ", sizeof("return ") - 1);
	memcpy(Z_STRVAL(pv) + sizeof("return ") - 1, expr, expr_len);
	Z_STRVAL(pv)[Z_STRLEN(pv) - 1] = ';';
	Z_STRVAL(pv)[Z_STRLEN(pv)] = '\0';
	Z_TYPE(pv) = IS_STRING;

	new_break.ops = zend_compile_string(&pv, "Conditional Breakpoint Code" TSRMLS_CC);

	zval_dtor(&pv);

	if (new_break.ops) {
		zend_hash_index_update(
			&PHPDBG_G(bp)[PHPDBG_BREAK_COND], hash, &new_break,
			sizeof(phpdbg_breakcond_t), (void**)&brake);

		phpdbg_notice("breakpoint", "id=\"%d\" expression=\"%s\" ptr=\"%p\"", "Conditional breakpoint #%d added %s/%p", brake->id, brake->code, brake->ops);

		PHPDBG_G(flags) |= PHPDBG_HAS_COND_BP;
		PHPDBG_BREAK_MAPPING(new_break.id, &PHPDBG_G(bp)[PHPDBG_BREAK_COND]);
	} else {
		 phpdbg_error("compile", "expression=\"%s\"", "Failed to compile code for expression %s", expr);
		 efree((char*)new_break.code);
		 PHPDBG_G(bp_count)--;
	}
	CG(compiler_options) = cops;
} /* }}} */

PHPDBG_API void phpdbg_set_breakpoint_expression(const char *expr, size_t expr_len TSRMLS_DC) /* {{{ */
{
	zend_ulong expr_hash = zend_inline_hash_func(expr, expr_len);
	phpdbg_breakcond_t new_break;

	if (!zend_hash_index_exists(&PHPDBG_G(bp)[PHPDBG_BREAK_COND], expr_hash)) {
		phpdbg_create_conditional_break(
			&new_break, NULL, expr, expr_len, expr_hash TSRMLS_CC);
	} else {
		phpdbg_error("breakpoint", "type=\"exists\" expression=\"%s\"", "Conditional break %s exists", expr);
	}
} /* }}} */

PHPDBG_API void phpdbg_set_breakpoint_at(const phpdbg_param_t *param TSRMLS_DC) /* {{{ */
{
	phpdbg_breakcond_t new_break;
	phpdbg_param_t *condition;
	zend_ulong hash = 0L;

	if (param->next) {
		condition = param->next;
		hash = zend_inline_hash_func(condition->str, condition->len);

		if (!zend_hash_index_exists(&PHPDBG_G(bp)[PHPDBG_BREAK_COND], hash)) {
			phpdbg_create_conditional_break(&new_break, param, condition->str, condition->len, hash TSRMLS_CC);
		} else {
			phpdbg_notice("breakpoint", "type=\"exists\" arg=\"%s\"", "Conditional break %s exists at the specified location", condition->str);
		}
	}

} /* }}} */

static inline phpdbg_breakbase_t *phpdbg_find_breakpoint_file(zend_op_array *op_array TSRMLS_DC) /* {{{ */
{
	HashTable *breaks;
	phpdbg_breakbase_t *brake;
	size_t path_len;
	char realpath[MAXPATHLEN];
	const char *path = op_array->filename;

	if (VCWD_REALPATH(path, realpath)) {
		path = realpath;
	}

	path_len = strlen(path);

#if 0
	phpdbg_debug("Op at: %.*s %d\n", path_len, path, (*EG(opline_ptr))->lineno);
#endif

	if (zend_hash_find(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE], path, path_len, (void**)&breaks) == FAILURE) {
		return NULL;
	}

	if (zend_hash_index_find(breaks, (*EG(opline_ptr))->lineno, (void**)&brake) == SUCCESS) {
		return brake;
	}

	return NULL;
} /* }}} */

static inline phpdbg_breakbase_t *phpdbg_find_breakpoint_symbol(zend_function *fbc TSRMLS_DC) /* {{{ */
{
	const char *fname;
	zend_op_array *ops;
	phpdbg_breakbase_t *brake;

	if (fbc->type != ZEND_USER_FUNCTION) {
		return NULL;
	}

	ops = (zend_op_array*)fbc;

	if (ops->scope) {
		/* find method breaks here */
		return phpdbg_find_breakpoint_method(ops TSRMLS_CC);
	}

	fname = ops->function_name;

	if (!fname) {
		fname = "main";
	}

	if (zend_hash_find(&PHPDBG_G(bp)[PHPDBG_BREAK_SYM], fname, strlen(fname), (void**)&brake) == SUCCESS) {
		return brake;
	}

	return NULL;
} /* }}} */

static inline phpdbg_breakbase_t *phpdbg_find_breakpoint_method(zend_op_array *ops TSRMLS_DC) /* {{{ */
{
	HashTable *class_table;
	phpdbg_breakbase_t *brake;

	if (zend_hash_find(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD], ops->scope->name,
		ops->scope->name_length, (void**)&class_table) == SUCCESS) {
		char *lcname = zend_str_tolower_dup(ops->function_name, strlen(ops->function_name));
		size_t lcname_len = strlen(lcname);

		if (zend_hash_find(
		        class_table,
		        lcname,
		        lcname_len, (void**)&brake) == SUCCESS) {
			efree(lcname);
			return brake;
		}

		efree(lcname);
	}

	return NULL;
} /* }}} */

static inline phpdbg_breakbase_t *phpdbg_find_breakpoint_opline(phpdbg_opline_ptr_t opline TSRMLS_DC) /* {{{ */
{
	phpdbg_breakline_t *brake;

	if (zend_hash_index_find(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE],
		(zend_ulong) opline, (void**)&brake) == SUCCESS) {
		return (brake->base?(phpdbg_breakbase_t *)brake->base:(phpdbg_breakbase_t *)brake);
	}

	return NULL;
} /* }}} */

static inline phpdbg_breakbase_t *phpdbg_find_breakpoint_opcode(zend_uchar opcode TSRMLS_DC) /* {{{ */
{
	phpdbg_breakbase_t *brake;
	const char *opname = phpdbg_decode_opcode(opcode);

	if (memcmp(opname, PHPDBG_STRL("UNKNOWN")) == 0) {
		return NULL;
	}

	if (zend_hash_index_find(&PHPDBG_G(bp)[PHPDBG_BREAK_OPCODE],
		zend_hash_func(opname, strlen(opname)), (void**)&brake) == SUCCESS) {
		return brake;
	}
	return NULL;
} /* }}} */

static inline zend_bool phpdbg_find_breakpoint_param(phpdbg_param_t *param, zend_execute_data *execute_data TSRMLS_DC) /* {{{ */
{
	zend_function *function = (zend_function*) execute_data->function_state.function;

	switch (param->type) {
		case NUMERIC_FUNCTION_PARAM:
		case STR_PARAM: {
			/* function breakpoint */

			if (function->type != ZEND_USER_FUNCTION) {
				return 0;
			}

			{
				const char *str = NULL;
				size_t len = 0L;
				zend_op_array *ops = (zend_op_array*)function;
				str = ops->function_name ? ops->function_name : "main";
				len = strlen(str);

				if (len == param->len && memcmp(param->str, str, len) == SUCCESS) {
					return param->type == STR_PARAM || execute_data->opline - ops->opcodes == param->num;
				}
			}
		} break;

		case FILE_PARAM: {
			if (param->file.line == zend_get_executed_lineno(TSRMLS_C)) {
				const char *str = zend_get_executed_filename(TSRMLS_C);
				size_t lengths[2] = {strlen(param->file.name), strlen(str)};

				if (lengths[0] == lengths[1]) {
					return (memcmp(
						param->file.name, str, lengths[0]) == SUCCESS);
				}
			}
		} break;

		case NUMERIC_METHOD_PARAM:
		case METHOD_PARAM: {
			if (function->type != ZEND_USER_FUNCTION) {
				return 0;
			}

			{
				zend_op_array *ops = (zend_op_array*) function;

				if (ops->scope) {
					size_t lengths[2] = {strlen(param->method.class), ops->scope->name_length};
					if (lengths[0] == lengths[1] && memcmp(param->method.class, ops->scope->name, lengths[0]) == SUCCESS) {
						lengths[0] = strlen(param->method.name);
						lengths[1] = strlen(ops->function_name);

						if (lengths[0] == lengths[1] && memcmp(param->method.name, ops->function_name, lengths[0]) == SUCCESS) {
							return param->type == METHOD_PARAM || (execute_data->opline - ops->opcodes) == param->num;
						}
					}
				}
			}
		} break;

		case ADDR_PARAM: {
			return ((zend_ulong)(phpdbg_opline_ptr_t)execute_data->opline == param->addr);
		} break;

		default: {
			/* do nothing */
		} break;
	}
	return 0;
} /* }}} */

static inline phpdbg_breakbase_t *phpdbg_find_conditional_breakpoint(zend_execute_data *execute_data TSRMLS_DC) /* {{{ */
{
	phpdbg_breakcond_t *bp;
	HashPosition position;
	int breakpoint = FAILURE;

	for (zend_hash_internal_pointer_reset_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_COND], &position);
	     zend_hash_get_current_data_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_COND], (void*)&bp, &position) == SUCCESS;
	     zend_hash_move_forward_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_COND], &position)) {
		zval *retval = NULL;
		zval **orig_retval = EG(return_value_ptr_ptr);
		zend_op_array *orig_ops = EG(active_op_array);
		zend_op **orig_opline = EG(opline_ptr);

		if (((phpdbg_breakbase_t*)bp)->disabled) {
			continue;
		}

		if (bp->paramed) {
			if (!phpdbg_find_breakpoint_param(&bp->param, execute_data TSRMLS_CC)) {
				continue;
			}
		}

		ALLOC_INIT_ZVAL(retval);

		EG(return_value_ptr_ptr) = &retval;
		EG(active_op_array) = bp->ops;
		EG(no_extensions) = 1;

		if (!EG(active_symbol_table)) {
			zend_rebuild_symbol_table(TSRMLS_C);
		}

		zend_try {
			PHPDBG_G(flags) |= PHPDBG_IN_COND_BP;
			zend_execute(EG(active_op_array) TSRMLS_CC);
#if PHP_VERSION_ID >= 50700
			if (zend_is_true(retval TSRMLS_CC)) {
#else
			if (zend_is_true(retval)) {
#endif
				breakpoint = SUCCESS;
			}
		} zend_catch {
			EG(no_extensions)=1;
			EG(return_value_ptr_ptr) = orig_retval;
			EG(active_op_array) = orig_ops;
			EG(opline_ptr) = orig_opline;
			PHPDBG_G(flags) &= ~PHPDBG_IN_COND_BP;
 		} zend_end_try();

		EG(no_extensions)=1;
		EG(return_value_ptr_ptr) = orig_retval;
		EG(active_op_array) = orig_ops;
		EG(opline_ptr) = orig_opline;
		PHPDBG_G(flags) &= ~PHPDBG_IN_COND_BP;

		if (breakpoint == SUCCESS) {
			break;
		}
	}

	return (breakpoint == SUCCESS) ? ((phpdbg_breakbase_t*)bp) : NULL;
} /* }}} */

PHPDBG_API phpdbg_breakbase_t *phpdbg_find_breakpoint(zend_execute_data* execute_data TSRMLS_DC) /* {{{ */
{
	phpdbg_breakbase_t *base = NULL;

	if (!(PHPDBG_G(flags) & PHPDBG_IS_BP_ENABLED)) {
		return NULL;
	}

	/* conditions cannot be executed by eval()'d code */
	if (!(PHPDBG_G(flags) & PHPDBG_IN_EVAL) &&
		(PHPDBG_G(flags) & PHPDBG_HAS_COND_BP) &&
		(base = phpdbg_find_conditional_breakpoint(execute_data TSRMLS_CC))) {
		goto result;
	}

	if ((PHPDBG_G(flags) & PHPDBG_HAS_FILE_BP) &&
		(base = phpdbg_find_breakpoint_file(execute_data->op_array TSRMLS_CC))) {
		goto result;
	}

	if (PHPDBG_G(flags) & (PHPDBG_HAS_METHOD_BP|PHPDBG_HAS_SYM_BP)) {
		/* check we are at the beginning of the stack */
		if (execute_data->opline == EG(active_op_array)->opcodes) {
			if ((base = phpdbg_find_breakpoint_symbol(
					execute_data->function_state.function TSRMLS_CC))) {
				goto result;
			}
		}
	}

	if ((PHPDBG_G(flags) & PHPDBG_HAS_OPLINE_BP) &&
		(base = phpdbg_find_breakpoint_opline(execute_data->opline TSRMLS_CC))) {
		goto result;
	}

	if ((PHPDBG_G(flags) & PHPDBG_HAS_OPCODE_BP) &&
		(base = phpdbg_find_breakpoint_opcode(execute_data->opline->opcode TSRMLS_CC))) {
		goto result;
	}

	return NULL;

result:
	/* we return nothing for disable breakpoints */
	if (base->disabled) {
		return NULL;
	}

	return base;
} /* }}} */

PHPDBG_API void phpdbg_delete_breakpoint(zend_ulong num TSRMLS_DC) /* {{{ */
{
	HashTable **table;
	HashPosition position;
	phpdbg_breakbase_t *brake;

	if ((brake = phpdbg_find_breakbase_ex(num, &table, &position TSRMLS_CC))) {
		char *key;
		zend_uint klen;
		zend_ulong idx;
		int type = brake->type;
		char *name = NULL;
		size_t name_len = 0L;

		switch (type) {
			case PHPDBG_BREAK_FILE:
			case PHPDBG_BREAK_METHOD:
				if (zend_hash_num_elements((*table)) == 1) {
					name = estrdup(brake->name);
					name_len = strlen(name);
					if (zend_hash_num_elements(&PHPDBG_G(bp)[type]) == 1) {
						PHPDBG_G(flags) &= ~(1<<(brake->type+1));
					}
				}
			break;

			default: {
				if (zend_hash_num_elements((*table)) == 1) {
					PHPDBG_G(flags) &= ~(1<<(brake->type+1));
				}
			}
		}

		switch (type) {
			case PHPDBG_BREAK_FILE_OPLINE:
			case PHPDBG_BREAK_FUNCTION_OPLINE:
			case PHPDBG_BREAK_METHOD_OPLINE:
				if (zend_hash_num_elements(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE]) == 1) {
					PHPDBG_G(flags) &= PHPDBG_HAS_OPLINE_BP;
				}
				zend_hash_index_del(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE], ((phpdbg_breakopline_t*)brake)->opline);
		}

		switch (zend_hash_get_current_key_ex(
			(*table), &key, &klen, &idx, 0, &position)) {

			case HASH_KEY_IS_STRING:
				zend_hash_del((*table), key, klen);
			break;

			default:
				zend_hash_index_del((*table), idx);
		}

		switch (type) {
			case PHPDBG_BREAK_FILE:
			case PHPDBG_BREAK_METHOD:
				if (name) {
					zend_hash_del(&PHPDBG_G(bp)[type], name, name_len);
					efree(name);
				}
			break;
		}

		phpdbg_notice("breakpoint", "deleted=\"success\" id=\"%ld\"", "Deleted breakpoint #%ld", num);
		PHPDBG_BREAK_UNMAPPING(num);
	} else {
		phpdbg_error("breakpoint", "type=\"nobreakpoint\" deleted=\"fail\" id=\"%ld\"", "Failed to find breakpoint #%ld", num);
	}
} /* }}} */

PHPDBG_API void phpdbg_clear_breakpoints(TSRMLS_D) /* {{{ */
{
	zend_hash_clean(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE]);
	zend_hash_clean(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_PENDING]);
	zend_hash_clean(&PHPDBG_G(bp)[PHPDBG_BREAK_SYM]);
	zend_hash_clean(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE]);
	zend_hash_clean(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD_OPLINE]);
	zend_hash_clean(&PHPDBG_G(bp)[PHPDBG_BREAK_FUNCTION_OPLINE]);
	zend_hash_clean(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_OPLINE]);
	zend_hash_clean(&PHPDBG_G(bp)[PHPDBG_BREAK_OPCODE]);
	zend_hash_clean(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD]);
	zend_hash_clean(&PHPDBG_G(bp)[PHPDBG_BREAK_COND]);
	zend_hash_clean(&PHPDBG_G(bp)[PHPDBG_BREAK_MAP]);

	PHPDBG_G(flags) &= ~PHPDBG_BP_MASK;

	PHPDBG_G(bp_count) = 0;
} /* }}} */

PHPDBG_API void phpdbg_hit_breakpoint(phpdbg_breakbase_t *brake, zend_bool output TSRMLS_DC) /* {{{ */
{
	brake->hits++;

	if (output) {
		phpdbg_print_breakpoint(brake TSRMLS_CC);
	}
} /* }}} */

PHPDBG_API void phpdbg_print_breakpoint(phpdbg_breakbase_t *brake TSRMLS_DC) /* {{{ */
{
	if (!brake)
		goto unknown;

	switch (brake->type) {
		case PHPDBG_BREAK_FILE: {
			phpdbg_notice("breakpoint", "id=\"%d\" file=\"%s\" line=\"%ld\" hits=\"%lu\"", "Breakpoint #%d at %s:%ld, hits: %lu",
				((phpdbg_breakfile_t*)brake)->id,
				((phpdbg_breakfile_t*)brake)->filename,
				((phpdbg_breakfile_t*)brake)->line,
				((phpdbg_breakfile_t*)brake)->hits);
		} break;

		case PHPDBG_BREAK_SYM: {
			phpdbg_notice("breakpoint", "id=\"%d\" function=\"%s\" file=\"%s\" line=\"%ld\" hits=\"%lu\"", "Breakpoint #%d in %s() at %s:%u, hits: %lu",
				((phpdbg_breaksymbol_t*)brake)->id,
				((phpdbg_breaksymbol_t*)brake)->symbol,
				zend_get_executed_filename(TSRMLS_C),
				zend_get_executed_lineno(TSRMLS_C),
				((phpdbg_breakfile_t*)brake)->hits);
		} break;

		case PHPDBG_BREAK_OPLINE: {
			phpdbg_notice("breakpoint", "id=\"%d\" opline=\"%#lx\" file=\"%s\" line=\"%ld\" hits=\"%lu\"", "Breakpoint #%d in %#lx at %s:%u, hits: %lu",
				((phpdbg_breakline_t*)brake)->id,
				((phpdbg_breakline_t*)brake)->opline,
				zend_get_executed_filename(TSRMLS_C),
				zend_get_executed_lineno(TSRMLS_C),
				((phpdbg_breakline_t*)brake)->hits);
		} break;

		case PHPDBG_BREAK_METHOD_OPLINE: {
			 phpdbg_notice("breakpoint", "id=\"%d\" method=\"%s::%s\" num=\"%lu\" file=\"%s\" line=\"%ld\" hits=\"%lu\"", "Breakpoint #%d in %s::%s()#%lu at %s:%u, hits: %lu",
				((phpdbg_breakopline_t*)brake)->id,
				((phpdbg_breakopline_t*)brake)->class_name,
				((phpdbg_breakopline_t*)brake)->func_name,
				((phpdbg_breakopline_t*)brake)->opline_num,
				zend_get_executed_filename(TSRMLS_C),
				zend_get_executed_lineno(TSRMLS_C),
				((phpdbg_breakopline_t*)brake)->hits);
		} break;

		case PHPDBG_BREAK_FUNCTION_OPLINE: {
			 phpdbg_notice("breakpoint", "id=\"%d\" num=\"%lu\" function=\"%s\" file=\"%s\" line=\"%ld\" hits=\"%lu\"", "Breakpoint #%d in %s()#%lu at %s:%u, hits: %lu",
				((phpdbg_breakopline_t*)brake)->id,
				((phpdbg_breakopline_t*)brake)->func_name,
				((phpdbg_breakopline_t*)brake)->opline_num,
				zend_get_executed_filename(TSRMLS_C),
				zend_get_executed_lineno(TSRMLS_C),
				((phpdbg_breakopline_t*)brake)->hits);
		} break;

		case PHPDBG_BREAK_FILE_OPLINE: {
			 phpdbg_notice("breakpoint", "id=\"%d\" num=\"%lu\" file=\"%s\" line=\"%ld\" hits=\"%lu\"", "Breakpoint #%d in #%lu at %s:%u, hits: %lu",
				((phpdbg_breakopline_t*)brake)->id,
				((phpdbg_breakopline_t*)brake)->opline_num,
				zend_get_executed_filename(TSRMLS_C),
				zend_get_executed_lineno(TSRMLS_C),
				((phpdbg_breakopline_t*)brake)->hits);
		} break;

		case PHPDBG_BREAK_OPCODE: {
			 phpdbg_notice("breakpoint", "id=\"%d\" opcode=\"%s\" file=\"%s\" line=\"%ld\" hits=\"%lu\"", "Breakpoint #%d in %s at %s:%u, hits: %lu",
				((phpdbg_breakop_t*)brake)->id,
				((phpdbg_breakop_t*)brake)->name,
				zend_get_executed_filename(TSRMLS_C),
				zend_get_executed_lineno(TSRMLS_C),
				((phpdbg_breakop_t*)brake)->hits);
		} break;

		case PHPDBG_BREAK_METHOD: {
			 phpdbg_notice("breakpoint", "id=\"%d\" method=\"%s::%s\" file=\"%s\" line=\"%ld\" hits=\"%lu\"", "Breakpoint #%d in %s::%s() at %s:%u, hits: %lu",
				((phpdbg_breakmethod_t*)brake)->id,
				((phpdbg_breakmethod_t*)brake)->class_name,
				((phpdbg_breakmethod_t*)brake)->func_name,
				zend_get_executed_filename(TSRMLS_C),
				zend_get_executed_lineno(TSRMLS_C),
				((phpdbg_breakmethod_t*)brake)->hits);
		} break;

		case PHPDBG_BREAK_COND: {
			if (((phpdbg_breakcond_t*)brake)->paramed) {
				char *param;
				phpdbg_notice("breakpoint", "id=\"%d\" location=\"%s\" eval=\"%s\" file=\"%s\" line=\"%ld\" hits=\"%lu\"", "Conditional breakpoint #%d: at %s if %s at %s:%u, hits: %lu",
					((phpdbg_breakcond_t*)brake)->id,
					phpdbg_param_tostring(&((phpdbg_breakcond_t*)brake)->param, &param TSRMLS_CC),
					((phpdbg_breakcond_t*)brake)->code,
					zend_get_executed_filename(TSRMLS_C),
					zend_get_executed_lineno(TSRMLS_C),
					((phpdbg_breakcond_t*)brake)->hits);
				if (param)
					free(param);
			} else {
				phpdbg_notice("breakpoint", "id=\"%d\" eval=\"%s\" file=\"%s\" line=\"%ld\" hits=\"%lu\"", "Conditional breakpoint #%d: on %s == true at %s:%u, hits: %lu",
					((phpdbg_breakcond_t*)brake)->id,
					((phpdbg_breakcond_t*)brake)->code,
					zend_get_executed_filename(TSRMLS_C),
					zend_get_executed_lineno(TSRMLS_C),
					((phpdbg_breakcond_t*)brake)->hits);
			}

		} break;

		default: {
unknown:
			phpdbg_notice("breakpoint", "id=\"\" file=\"%s\" line=\"%ld\" hits=\"%lu\"", "Unknown breakpoint at %s:%u",
				zend_get_executed_filename(TSRMLS_C),
				zend_get_executed_lineno(TSRMLS_C));
		}
	}
} /* }}} */

PHPDBG_API void phpdbg_enable_breakpoint(zend_ulong id TSRMLS_DC) /* {{{ */
{
	phpdbg_breakbase_t *brake = phpdbg_find_breakbase(id TSRMLS_CC);

	if (brake) {
		brake->disabled = 0;
	}
} /* }}} */

PHPDBG_API void phpdbg_disable_breakpoint(zend_ulong id TSRMLS_DC) /* {{{ */
{
	phpdbg_breakbase_t *brake = phpdbg_find_breakbase(id TSRMLS_CC);

	if (brake) {
		brake->disabled = 1;
	}
} /* }}} */

PHPDBG_API void phpdbg_enable_breakpoints(TSRMLS_D) /* {{{ */
{
	PHPDBG_G(flags) |= PHPDBG_IS_BP_ENABLED;
} /* }}} */

PHPDBG_API void phpdbg_disable_breakpoints(TSRMLS_D) { /* {{{ */
	PHPDBG_G(flags) &= ~PHPDBG_IS_BP_ENABLED;
} /* }}} */

PHPDBG_API phpdbg_breakbase_t *phpdbg_find_breakbase(zend_ulong id TSRMLS_DC) /* {{{ */
{
	HashTable **table;
	HashPosition position;

	return phpdbg_find_breakbase_ex(id, &table, &position TSRMLS_CC);
} /* }}} */

PHPDBG_API phpdbg_breakbase_t *phpdbg_find_breakbase_ex(zend_ulong id, HashTable ***table, HashPosition *position TSRMLS_DC) /* {{{ */
{
	if (zend_hash_index_find(&PHPDBG_G(bp)[PHPDBG_BREAK_MAP], id, (void**)table) == SUCCESS) {
		phpdbg_breakbase_t *brake;

		for (zend_hash_internal_pointer_reset_ex((**table), position);
			zend_hash_get_current_data_ex((**table), (void**)&brake, position) == SUCCESS;
			zend_hash_move_forward_ex((**table), position)) {

			if (brake->id == id) {
				return brake;
			}
		}
	}
	return NULL;
} /* }}} */

PHPDBG_API void phpdbg_print_breakpoints(zend_ulong type TSRMLS_DC) /* {{{ */
{
	phpdbg_xml("<breakpoints %r>");

	switch (type) {
		case PHPDBG_BREAK_SYM: if ((PHPDBG_G(flags) & PHPDBG_HAS_SYM_BP)) {
			HashPosition position;
			phpdbg_breaksymbol_t *brake;

			phpdbg_out(SEPARATE "\n");
			phpdbg_out("Function Breakpoints:\n");
			for (zend_hash_internal_pointer_reset_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_SYM], &position);
			     zend_hash_get_current_data_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_SYM], (void**) &brake, &position) == SUCCESS;
			     zend_hash_move_forward_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_SYM], &position)) {
				phpdbg_writeln("function", "id=\"%d\" name=\"%s\" disabled=\"%s\"", "#%d\t\t%s%s",
					brake->id, brake->symbol,
					((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
			}
		} break;

		case PHPDBG_BREAK_METHOD: if ((PHPDBG_G(flags) & PHPDBG_HAS_METHOD_BP)) {
			HashPosition position[2];
			HashTable *class_table;
			char *class_name = NULL;
			zend_uint class_len = 0;
			zend_ulong class_idx = 0L;

			phpdbg_out(SEPARATE "\n");
			phpdbg_out("Method Breakpoints:\n");
			for (zend_hash_internal_pointer_reset_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD], &position[0]);
			     zend_hash_get_current_data_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD], (void**) &class_table, &position[0]) == SUCCESS;
			     zend_hash_move_forward_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD], &position[0])) {

				if (zend_hash_get_current_key_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD],
					&class_name, &class_len, &class_idx, 0, &position[0]) == HASH_KEY_IS_STRING) {
					phpdbg_breakmethod_t *brake;

					for (zend_hash_internal_pointer_reset_ex(class_table, &position[1]);
					     zend_hash_get_current_data_ex(class_table, (void**)&brake, &position[1]) == SUCCESS;
					     zend_hash_move_forward_ex(class_table, &position[1])) {
						phpdbg_writeln("method", "id=\"%d\" name=\"%s::%s\" disabled=\"%s\"", "#%d\t\t%s::%s%s",
							brake->id, brake->class_name, brake->func_name,
							((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
					}
				}

			}
		} break;

		case PHPDBG_BREAK_FILE: if ((PHPDBG_G(flags) & PHPDBG_HAS_FILE_BP)) {
			HashPosition position[2];
			HashTable *points;

			phpdbg_out(SEPARATE "\n");
			phpdbg_out("File Breakpoints:\n");
			for (zend_hash_internal_pointer_reset_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE], &position[0]);
			     zend_hash_get_current_data_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE], (void**) &points, &position[0]) == SUCCESS;
			     zend_hash_move_forward_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE], &position[0])) {
				phpdbg_breakfile_t *brake;

				for (zend_hash_internal_pointer_reset_ex(points, &position[1]);
				     zend_hash_get_current_data_ex(points, (void**)&brake, &position[1]) == SUCCESS;
				     zend_hash_move_forward_ex(points, &position[1])) {
					phpdbg_writeln("file", "id=\"%d\" name=\"%s\" line=\"%lu\" disabled=\"%s\"", "#%d\t\t%s:%lu%s",
						brake->id, brake->filename, brake->line,
						((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
				}
			}
		}  if ((PHPDBG_G(flags) & PHPDBG_HAS_PENDING_FILE_BP)) {
			HashPosition position[2];
			HashTable *points;

			phpdbg_out(SEPARATE "\n");
			phpdbg_out("Pending File Breakpoints:\n");
			for (zend_hash_internal_pointer_reset_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_PENDING], &position[0]);
			     zend_hash_get_current_data_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_PENDING], (void**) &points, &position[0]) == SUCCESS;
			     zend_hash_move_forward_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_PENDING], &position[0])) {
				phpdbg_breakfile_t *brake;

				for (zend_hash_internal_pointer_reset_ex(points, &position[1]);
				     zend_hash_get_current_data_ex(points, (void**)&brake, &position[1]) == SUCCESS;
				     zend_hash_move_forward_ex(points, &position[1])) {
					phpdbg_writeln("file", "id=\"%d\" name=\"%s\" line=\"%lu\" disabled=\"%s\" pending=\"pending\"", "#%d\t\t%s:%lu%s",
						brake->id, brake->filename, brake->line,
						((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
				}
			}

		} break;

		case PHPDBG_BREAK_OPLINE: if ((PHPDBG_G(flags) & PHPDBG_HAS_OPLINE_BP)) {
			HashPosition position;
			phpdbg_breakline_t *brake;

			phpdbg_out(SEPARATE "\n");
			phpdbg_out("Opline Breakpoints:\n");
			for (zend_hash_internal_pointer_reset_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE], &position);
			     zend_hash_get_current_data_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE], (void**) &brake, &position) == SUCCESS;
			     zend_hash_move_forward_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_OPLINE], &position)) {
				switch (brake->type) {
					case PHPDBG_BREAK_METHOD_OPLINE:
					case PHPDBG_BREAK_FUNCTION_OPLINE:
					case PHPDBG_BREAK_FILE_OPLINE:
						phpdbg_writeln("opline", "id=\"%d\" num=\"%#lx\" type=\"%s\" disabled=\"%s\"", "#%d\t\t%#lx\t\t(%s breakpoint)%s", brake->id, brake->opline,
							brake->type == PHPDBG_BREAK_METHOD_OPLINE ? "method" :
								brake->type == PHPDBG_BREAK_FUNCTION_OPLINE ? "function" :
									brake->type == PHPDBG_BREAK_FILE_OPLINE ? "file" :
										"--- error ---",
							((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
						break;

					default:
						phpdbg_writeln("opline", "id=\"%d\" num=\"%#lx\" disabled=\"%s\"", "#%d\t\t%#lx%s", brake->id, brake->opline, ((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
						break;
				}
			}
		} break;

		case PHPDBG_BREAK_METHOD_OPLINE: if ((PHPDBG_G(flags) & PHPDBG_HAS_METHOD_OPLINE_BP)) {
			HashPosition position[3];
			HashTable *class_table, *method_table;
			char *class_name = NULL, *method_name = NULL;
			zend_uint class_len = 0, method_len = 0;
			zend_ulong class_idx = 0L, method_idx = 0L;

			phpdbg_out(SEPARATE "\n");
			phpdbg_out("Method opline Breakpoints:\n");
			for (zend_hash_internal_pointer_reset_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD_OPLINE], &position[0]);
			     zend_hash_get_current_data_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD_OPLINE], (void**) &class_table, &position[0]) == SUCCESS;
			     zend_hash_move_forward_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD_OPLINE], &position[0])) {

				if (zend_hash_get_current_key_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_METHOD_OPLINE],
					&class_name, &class_len, &class_idx, 0, &position[0]) == HASH_KEY_IS_STRING) {

					for (zend_hash_internal_pointer_reset_ex(class_table, &position[1]);
					     zend_hash_get_current_data_ex(class_table, (void**) &method_table, &position[1]) == SUCCESS;
					     zend_hash_move_forward_ex(class_table, &position[1])) {

						if (zend_hash_get_current_key_ex(class_table,
							&method_name, &method_len, &method_idx, 0, &position[0]) == HASH_KEY_IS_STRING) {

							phpdbg_breakopline_t *brake;

							for (zend_hash_internal_pointer_reset_ex(method_table, &position[2]);
							     zend_hash_get_current_data_ex(method_table, (void**)&brake, &position[2]) == SUCCESS;
							     zend_hash_move_forward_ex(method_table, &position[2])) {
								phpdbg_writeln("methodopline", "id=\"%d\" name=\"%s::%s\" num=\"%ld\" disabled=\"%s\"", "#%d\t\t%s::%s opline %ld%s",
									brake->id, brake->class_name, brake->func_name, brake->opline_num,
									((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
							}
						}
					}
				}

			}
		} break;

		case PHPDBG_BREAK_FUNCTION_OPLINE: if ((PHPDBG_G(flags) & PHPDBG_HAS_FUNCTION_OPLINE_BP)) {
			HashPosition position[2];
			HashTable *function_table;
			char *function_name = NULL;
			zend_uint function_len = 0;
			zend_ulong function_idx = 0L;

			phpdbg_out(SEPARATE "\n");
			phpdbg_out("Function opline Breakpoints:\n");
			for (zend_hash_internal_pointer_reset_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FUNCTION_OPLINE], &position[0]);
			     zend_hash_get_current_data_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FUNCTION_OPLINE], (void**) &function_table, &position[0]) == SUCCESS;
			     zend_hash_move_forward_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FUNCTION_OPLINE], &position[0])) {

				if (zend_hash_get_current_key_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FUNCTION_OPLINE],
					&function_name, &function_len, &function_idx, 0, &position[0]) == HASH_KEY_IS_STRING) {

					phpdbg_breakopline_t *brake;

					for (zend_hash_internal_pointer_reset_ex(function_table, &position[1]);
					     zend_hash_get_current_data_ex(function_table, (void**)&brake, &position[1]) == SUCCESS;
					     zend_hash_move_forward_ex(function_table, &position[1])) {
						phpdbg_writeln("functionopline", "id=\"%d\" name=\"%s\" num=\"%ld\" disabled=\"%s\"", "#%d\t\t%s opline %ld%s",
							brake->id, brake->func_name, brake->opline_num,
							((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
					}
				}

			}
		} break;

		case PHPDBG_BREAK_FILE_OPLINE: if ((PHPDBG_G(flags) & PHPDBG_HAS_FILE_OPLINE_BP)) {
			HashPosition position[2];
			HashTable *file_table;
			char *file_name = NULL;
			zend_uint file_len = 0;
			zend_ulong file_idx = 0L;

			phpdbg_out(SEPARATE "\n");
			phpdbg_out("File opline Breakpoints:\n");
			for (zend_hash_internal_pointer_reset_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_OPLINE], &position[0]);
			     zend_hash_get_current_data_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_OPLINE], (void**) &file_table, &position[0]) == SUCCESS;
			     zend_hash_move_forward_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_OPLINE], &position[0])) {

				if (zend_hash_get_current_key_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_FILE_OPLINE],
					&file_name, &file_len, &file_idx, 0, &position[0]) == HASH_KEY_IS_STRING) {

					phpdbg_breakopline_t *brake;

					for (zend_hash_internal_pointer_reset_ex(file_table, &position[1]);
					     zend_hash_get_current_data_ex(file_table, (void**)&brake, &position[1]) == SUCCESS;
					     zend_hash_move_forward_ex(file_table, &position[1])) {
						phpdbg_writeln("fileopline", "id=\"%d\" name=\"%s\" num=\"%ld\" disabled=\"%s\"", "#%d\t\t%s opline %ld%s",
							brake->id, brake->class_name, brake->opline_num,
							((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
					}
				}
			}
		} break;

		case PHPDBG_BREAK_COND: if ((PHPDBG_G(flags) & PHPDBG_HAS_COND_BP)) {
			HashPosition position;
			phpdbg_breakcond_t *brake;

			phpdbg_out(SEPARATE "\n");
			phpdbg_out("Conditional Breakpoints:\n");
			for (zend_hash_internal_pointer_reset_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_COND], &position);
			     zend_hash_get_current_data_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_COND], (void**) &brake, &position) == SUCCESS;
			     zend_hash_move_forward_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_COND], &position)) {
				if (brake->paramed) {
					switch (brake->param.type) {
						case STR_PARAM:
							phpdbg_writeln("evalfunction", "id=\"%d\" name=\"%s\" eval=\"%s\" disabled=\"%s\"", "#%d\t\tat %s if %s%s",
				 				brake->id,
				 				brake->param.str,
				 				brake->code,
				 				((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
						break;

						case NUMERIC_FUNCTION_PARAM:
							phpdbg_writeln("evalfunctionopline", "id=\"%d\" name=\"%s\" num=\"%ld\" eval=\"%s\" disabled=\"%s\"", "#%d\t\tat %s#%ld if %s%s",
				 				brake->id,
				 				brake->param.str,
								brake->param.num,
				 				brake->code,
				 				((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
						break;

						case METHOD_PARAM:
							phpdbg_writeln("evalmethod", "id=\"%d\" name=\"%s::%s\" eval=\"%s\" disabled=\"%s\"", "#%d\t\tat %s::%s if %s%s",
				 				brake->id,
				 				brake->param.method.class,
				 				brake->param.method.name,
				 				brake->code,
				 				((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
						break;

						case NUMERIC_METHOD_PARAM:
							phpdbg_writeln("evalmethodopline", "id=\"%d\" name=\"%s::%s\" num=\"%d\" eval=\"%s\" disabled=\"%s\"", "#%d\t\tat %s::%s#%ld if %s%s",
				 				brake->id,
				 				brake->param.method.class,
				 				brake->param.method.name,
								brake->param.num,
				 				brake->code,
				 				((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
						break;

						case FILE_PARAM:
							phpdbg_writeln("evalfile", "id=\"%d\" name=\"%s\" line=\"%d\" eval=\"%s\" disabled=\"%s\"", "#%d\t\tat %s:%lu if %s%s",
				 				brake->id,
				 				brake->param.file.name,
				 				brake->param.file.line,
				 				brake->code,
				 				((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
						break;

						case ADDR_PARAM:
							phpdbg_writeln("evalopline", "id=\"%d\" opline=\"%#lx\" eval=\"%s\" disabled=\"%s\"", "#%d\t\tat #%lx if %s%s",
				 				brake->id,
				 				brake->param.addr,
				 				brake->code,
				 				((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
						break;

						default:
							phpdbg_error("eval", "type=\"invalidparameter\"", "Invalid parameter type for conditional breakpoint");
						return;
					}
				} else {
					phpdbg_writeln("eval", "id=\"%d\" eval=\"%s\" disabled=\"%s\"", "#%d\t\tif %s%s",
				 		brake->id, brake->code,
				 		((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
				}
			}
		} break;

		case PHPDBG_BREAK_OPCODE: if (PHPDBG_G(flags) & PHPDBG_HAS_OPCODE_BP) {
			HashPosition position;
			phpdbg_breakop_t *brake;

			phpdbg_out(SEPARATE "\n");
			phpdbg_out("Opcode Breakpoints:\n");
			for (zend_hash_internal_pointer_reset_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_OPCODE], &position);
			     zend_hash_get_current_data_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_OPCODE], (void**) &brake, &position) == SUCCESS;
			     zend_hash_move_forward_ex(&PHPDBG_G(bp)[PHPDBG_BREAK_OPCODE], &position)) {
				phpdbg_writeln("opcode", "id=\"%d\" name=\"%s\" disabled=\"%s\"", "#%d\t\t%s%s",
					brake->id, brake->name,
					((phpdbg_breakbase_t*)brake)->disabled ? " [disabled]" : "");
			}
		} break;
	}

	phpdbg_xml("</breakpoints>");
} /* }}} */
