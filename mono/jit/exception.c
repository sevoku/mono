/*
 * exception.c: exception support
 *
 * Authors:
 *   Dietmar Maurer (dietmar@ximian.com)
 *
 * (C) 2001 Ximian, Inc.
 */

#include <config.h>
#include <glib.h>
#include <signal.h>

#include <mono/arch/x86/x86-codegen.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/debug-helpers.h>

#include "jit.h"
#include "codegen.h"
#include "debug.h"

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
# define SC_EAX sc_eax
# define SC_EBX sc_ebx
# define SC_ECX sc_ecx
# define SC_EDX sc_edx
# define SC_EBP sc_ebp
# define SC_EIP sc_eip
# define SC_ESP sc_esp
# define SC_EDI sc_edi
# define SC_ESI sc_esi
#else
# define SC_EAX eax
# define SC_EBX ebx
# define SC_ECX ecx
# define SC_EDX edx
# define SC_EBP ebp
# define SC_EIP eip
# define SC_ESP esp
# define SC_EDI edi
# define SC_ESI esi
#endif

typedef struct sigcontext MonoContext;

#define MONO_CONTEXT_SET_IP(ctx,ip) do { (ctx)->SC_EIP = (long)ip; } while (0); 
#define MONO_CONTEXT_SET_BP(ctx,bp) do { (ctx)->SC_EBP = (long)bp; } while (0); 
#define MONO_CONTEXT_SET_EXCREG(ctx,exc) do { (ctx)->SC_ECX = (long)exc; } while (0); 

#define MONO_CONTEXT_GET_IP(ctx) ((gpointer)((ctx)->SC_EIP))
#define MONO_CONTEXT_GET_BP(ctx) ((gpointer)((ctx)->SC_EBP))

/*
 * arch_get_restore_context:
 *
 * Returns a pointer to a method which restores a previously saved sigcontext.
 */
static gpointer
arch_get_restore_context (void)
{
	static guint8 *start = NULL;
	guint8 *code;

	if (start)
		return start;

	/* restore_contect (struct sigcontext *ctx) */
	/* we do not restore X86_EAX, X86_EDX */

	start = code = g_malloc (1024);
	
	/* load ctx */
	x86_mov_reg_membase (code, X86_EAX, X86_ESP, 4, 4);

	/* get return address, stored in EDX */
	x86_mov_reg_membase (code, X86_EDX, X86_EAX,  G_STRUCT_OFFSET (struct sigcontext, SC_EIP), 4);
	/* restore EBX */
	x86_mov_reg_membase (code, X86_EBX, X86_EAX,  G_STRUCT_OFFSET (struct sigcontext, SC_EBX), 4);
	/* restore EDI */
	x86_mov_reg_membase (code, X86_EDI, X86_EAX,  G_STRUCT_OFFSET (struct sigcontext, SC_EDI), 4);
	/* restore ESI */
	x86_mov_reg_membase (code, X86_ESI, X86_EAX,  G_STRUCT_OFFSET (struct sigcontext, SC_ESI), 4);
	/* restore ESP */
	x86_mov_reg_membase (code, X86_ESP, X86_EAX,  G_STRUCT_OFFSET (struct sigcontext, SC_ESP), 4);
	/* restore EBP */
	x86_mov_reg_membase (code, X86_EBP, X86_EAX,  G_STRUCT_OFFSET (struct sigcontext, SC_EBP), 4);
	/* restore ECX. the exception object is passed here to the catch handler */
	x86_mov_reg_membase (code, X86_ECX, X86_EAX,  G_STRUCT_OFFSET (struct sigcontext, SC_ECX), 4);

	/* jump to the saved IP */
	x86_jump_reg (code, X86_EDX);

	return start;
}

/*
 * arch_get_call_filter:
 *
 * Returns a pointer to a method which calls an exception filter. We
 * also use this function to call finally handlers (we pass NULL as 
 * @exc object in this case).
 */
static gpointer
arch_get_call_filter (void)
{
	static guint8 start [64];
	static int inited = 0;
	guint8 *code;

	if (inited)
		return start;

	inited = 1;
	/* call_filter (struct sigcontext *ctx, unsigned long eip, gpointer exc) */
	code = start;

	x86_push_reg (code, X86_EBP);
	x86_mov_reg_reg (code, X86_EBP, X86_ESP, 4);
	x86_push_reg (code, X86_EBX);
	x86_push_reg (code, X86_EDI);
	x86_push_reg (code, X86_ESI);

	/* load ctx */
	x86_mov_reg_membase (code, X86_EAX, X86_EBP, 8, 4);
	/* load eip */
	x86_mov_reg_membase (code, X86_ECX, X86_EBP, 12, 4);
	/* save EBP */
	x86_push_reg (code, X86_EBP);
	/* push exc */
	x86_push_membase (code, X86_EBP, 16);
	/* set new EBP */
	x86_mov_reg_membase (code, X86_EBP, X86_EAX,  G_STRUCT_OFFSET (struct sigcontext, SC_EBP), 4);
	/* restore registers used by global register allocation (EBX & ESI) */
	x86_mov_reg_membase (code, X86_EBX, X86_EAX,  G_STRUCT_OFFSET (struct sigcontext, SC_EBX), 4);
	x86_mov_reg_membase (code, X86_ESI, X86_EAX,  G_STRUCT_OFFSET (struct sigcontext, SC_ESI), 4);
	/* save the ESP - this is used by endfinally */
	x86_mov_membase_reg (code, X86_EBP, mono_exc_esp_offset, X86_ESP, 4);
	/* call the handler */
	x86_call_reg (code, X86_ECX);
	x86_alu_reg_imm (code, X86_ADD, X86_ESP, 4);
	/* restore EBP */
	x86_pop_reg (code, X86_EBP);
	/* restore saved regs */
	x86_pop_reg (code, X86_ESI);
	x86_pop_reg (code, X86_EDI);
	x86_pop_reg (code, X86_EBX);
	x86_leave (code);
	x86_ret (code);

	g_assert ((code - start) < 64);
	return start;
}

static void
throw_exception (unsigned long eax, unsigned long ecx, unsigned long edx, unsigned long ebx,
		 unsigned long esi, unsigned long edi, unsigned long ebp, MonoObject *exc,
		 unsigned long eip,  unsigned long esp)
{
	static void (*restore_context) (struct sigcontext *);
	struct sigcontext ctx;

	if (!restore_context)
		restore_context = arch_get_restore_context ();

	/* adjust eip so that it point into the call instruction */
	eip -= 1;

	ctx.SC_ESP = esp;
	ctx.SC_EIP = eip;
	ctx.SC_EBP = ebp;
	ctx.SC_EDI = edi;
	ctx.SC_ESI = esi;
	ctx.SC_EBX = ebx;
	ctx.SC_EDX = edx;
	ctx.SC_ECX = ecx;
	ctx.SC_EAX = eax;
	
	arch_handle_exception (&ctx, exc, FALSE);
	restore_context (&ctx);

	g_assert_not_reached ();
}

/**
 * arch_get_throw_exception:
 *
 * Returns a function pointer which can be used to raise 
 * exceptions. The returned function has the following 
 * signature: void (*func) (MonoException *exc); 
 * For example to raise an arithmetic exception you can use:
 *
 * x86_push_imm (code, mono_get_exception_arithmetic ()); 
 * x86_call_code (code, arch_get_throw_exception ()); 
 *
 */
gpointer 
arch_get_throw_exception (void)
{
	static guint8 start [24];
	static int inited = 0;
	guint8 *code;

	if (inited)
		return start;

	inited = 1;
	code = start;

	x86_push_reg (code, X86_ESP);
	x86_push_membase (code, X86_ESP, 4); /* IP */
	x86_push_membase (code, X86_ESP, 12); /* exception */
	x86_push_reg (code, X86_EBP);
	x86_push_reg (code, X86_EDI);
	x86_push_reg (code, X86_ESI);
	x86_push_reg (code, X86_EBX);
	x86_push_reg (code, X86_EDX);
	x86_push_reg (code, X86_ECX);
	x86_push_reg (code, X86_EAX);
	x86_call_code (code, throw_exception);
	/* we should never reach this breakpoint */
	x86_breakpoint (code);

	g_assert ((code - start) < 24);
	return start;
}

/**
 * arch_get_throw_exception_by_name:
 *
 * Returns a function pointer which can be used to raise 
 * corlib exceptions. The returned function has the following 
 * signature: void (*func) (char *exc_name); 
 * For example to raise an arithmetic exception you can use:
 *
 * x86_push_imm (code, "ArithmeticException"); 
 * x86_call_code (code, arch_get_throw_exception ()); 
 *
 */
gpointer 
arch_get_throw_exception_by_name ()
{
	static guint8 start [32];
	static int inited = 0;
	guint8 *code;

	if (inited)
		return start;

	inited = 1;
	code = start;

	/* fixme: we do not save EAX, EDX, ECD - unsure if we need that */

	x86_push_membase (code, X86_ESP, 4); /* exception name */
	x86_push_imm (code, "System");
	x86_push_imm (code, mono_defaults.exception_class->image);
	x86_call_code (code, mono_exception_from_name);
	x86_alu_reg_imm (code, X86_ADD, X86_ESP, 12);
	/* save the newly create object (overwrite exception name)*/
	x86_mov_membase_reg (code, X86_ESP, 4, X86_EAX, 4);
	x86_jump_code (code, arch_get_throw_exception ());

	g_assert ((code - start) < 32);

	return start;
}	

static MonoArray *
glist_to_array (GList *list) 
{
	MonoDomain *domain = mono_domain_get ();
	MonoArray *res;
	int len, i;

	if (!list)
		return NULL;

	len = g_list_length (list);
	res = mono_array_new (domain, mono_defaults.int_class, len);

	for (i = 0; list; list = list->next, i++)
		mono_array_set (res, gpointer, i, list->data);

	return res;
}

/* mono_arch_find_jit_info:
 *
 * This function is used to gather information from @ctx. It return the 
 * MonoJitInfo of the corresponding function, unwinds one stack frame and
 * stores the resulting context into @new_ctx. It also stores a string 
 * describing the stack location into @trace (if not NULL), and modifies
 * the @lmf if necessary. @native_offset return the IP offset from the 
 * start of the function or -1 if that info is not available.
 */
static MonoJitInfo *
mono_arch_find_jit_info (MonoDomain *domain, MonoJitTlsData *jit_tls, MonoContext *ctx, 
			 MonoContext *new_ctx, char **trace, MonoLMF **lmf, int *native_offset)
{
	MonoJitInfo *ji;
	gpointer ip = MONO_CONTEXT_GET_IP (ctx);

	ji = mono_jit_info_table_find (domain, ip);

	if (trace)
		*trace = NULL;

	if (native_offset)
		*native_offset = -1;

	*new_ctx = *ctx;

	if (ji != NULL) {
		char *source_location, *tmpaddr, *fname;
		gint32 address, iloffset;
		int offset;

		if (*lmf && (MONO_CONTEXT_GET_BP (ctx) >= (gpointer)(*lmf)->ebp)) {
			/* remove any unused lmf */
			*lmf = (*lmf)->previous_lmf;
		}

		address = (char *)ip - (char *)ji->code_start;

		if (native_offset)
			*native_offset = address;

		if (trace) {
			if (mono_debug_format != MONO_DEBUG_FORMAT_NONE)
				mono_debug_make_symbols ();

			source_location = mono_debug_source_location_from_address (ji->method, address, NULL);
			iloffset = mono_debug_il_offset_from_address (ji->method, address);

			if (iloffset < 0)
				tmpaddr = g_strdup_printf ("<0x%05x>", address);
			else
				tmpaddr = g_strdup_printf ("[0x%05x]", iloffset);
		
			fname = mono_method_full_name (ji->method, TRUE);

			if (source_location)
				*trace = g_strdup_printf ("in %s (at %s) %s", tmpaddr, source_location, fname);
			else
				*trace = g_strdup_printf ("in %s %s", tmpaddr, fname);

			g_free (fname);
			g_free (source_location);
			g_free (tmpaddr);
		}
				
		offset = -1;
		/* restore caller saved registers */
		if (ji->used_regs & X86_EBX_MASK) {
			new_ctx->SC_EBX = *((int *)ctx->SC_EBP + offset);
			offset--;
		}
		if (ji->used_regs & X86_EDI_MASK) {
			new_ctx->SC_EDI = *((int *)ctx->SC_EBP + offset);
			offset--;
		}
		if (ji->used_regs & X86_ESI_MASK) {
			new_ctx->SC_ESI = *((int *)ctx->SC_EBP + offset);
		}

		new_ctx->SC_ESP = ctx->SC_EBP;
		/* we substract 1, so that the IP points into the call instruction */
		new_ctx->SC_EIP = *((int *)ctx->SC_EBP + 1) - 1;
		new_ctx->SC_EBP = *((int *)ctx->SC_EBP);

		return ji;

	} else if (*lmf) {
		
		if (trace)
			*trace = g_strdup_printf ("in (unmanaged) %s", mono_method_full_name ((*lmf)->method, TRUE));
		
		ji = mono_jit_info_table_find (domain, (gpointer)(*lmf)->eip);
		g_assert (ji != NULL);

		new_ctx->SC_ESI = (*lmf)->esi;
		new_ctx->SC_EDI = (*lmf)->edi;
		new_ctx->SC_EBX = (*lmf)->ebx;
		new_ctx->SC_EBP = (*lmf)->ebp;
		new_ctx->SC_EIP = (*lmf)->eip;
		/* the lmf is always stored on the stack, so the following
		 * expression points to a stack location which can be used as ESP */
		new_ctx->SC_ESP = (unsigned long)&((*lmf)->eip);

		*lmf = (*lmf)->previous_lmf;

		return ji;
		
	} else {
		/* no lmf available - usually a serious error? */
		new_ctx->SC_EBP = (unsigned long)jit_tls->end_of_stack;
		return NULL;
	}

	g_assert_not_reached ();
}

MonoArray *
ves_icall_get_trace (MonoException *exc, gint32 skip, MonoBoolean need_file_info)
{
	MonoDomain *domain = mono_domain_get ();
	MonoArray *res;
	MonoArray *ta = exc->trace_ips;
	int i, len;
	
	len = mono_array_length (ta);

	res = mono_array_new (domain, mono_defaults.stack_frame_class, len > skip ? len - skip : 0);

	for (i = skip; i < len; i++) {
		MonoJitInfo *ji;
		MonoStackFrame *sf = (MonoStackFrame *)mono_object_new (domain, mono_defaults.stack_frame_class);
		gpointer ip = mono_array_get (ta, gpointer, i);

		ji = mono_jit_info_table_find (domain, ip);
		g_assert (ji != NULL);

		sf->method = mono_method_get_object (domain, ji->method, NULL);
		sf->native_offset = (char *)ip - (char *)ji->code_start;
		sf->il_offset = mono_debug_il_offset_from_address (ji->method, sf->native_offset);

		if (need_file_info) {
			gchar *filename;

			filename = mono_debug_source_location_from_address (ji->method, sf->native_offset, &sf->line);

			sf->filename = mono_string_new (domain, filename ? filename : "<unknown>");
			sf->column = 0;

			g_free (filename);
		}

		mono_array_set (res, gpointer, i, sf);
	}

	return res;
}

void
mono_jit_walk_stack (MonoStackWalk func, gpointer user_data) {
	MonoDomain *domain = mono_domain_get ();
	MonoJitTlsData *jit_tls = TlsGetValue (mono_jit_tls_id);
	MonoLMF *lmf = jit_tls->lmf;
	MonoJitInfo *ji;
	gint native_offset, il_offset;

	MonoContext ctx, new_ctx;

	MONO_CONTEXT_SET_IP (&ctx, __builtin_return_address (0));
	MONO_CONTEXT_SET_BP (&ctx, __builtin_frame_address (1));

	while (MONO_CONTEXT_GET_BP (&ctx) < jit_tls->end_of_stack) {
		
		ji = mono_arch_find_jit_info (domain, jit_tls, &ctx, &new_ctx, NULL, &lmf, &native_offset);
		g_assert (ji);
		
		il_offset = mono_debug_il_offset_from_address (ji->method, native_offset);

		if (func (ji->method, native_offset, il_offset, user_data))
			return;
		
		ctx = new_ctx;
	}
}

MonoBoolean
ves_icall_get_frame_info (gint32 skip, MonoBoolean need_file_info, 
			  MonoReflectionMethod **method, 
			  gint32 *iloffset, gint32 *native_offset,
			  MonoString **file, gint32 *line, gint32 *column)
{
	MonoDomain *domain = mono_domain_get ();
	MonoJitTlsData *jit_tls = TlsGetValue (mono_jit_tls_id);
	MonoLMF *lmf = jit_tls->lmf;
	MonoJitInfo *ji;
	MonoContext ctx, new_ctx;

	MONO_CONTEXT_SET_IP (&ctx, ves_icall_get_frame_info);
	MONO_CONTEXT_SET_BP (&ctx, __builtin_frame_address (0));

	do {
		ji = mono_arch_find_jit_info (domain, jit_tls, &ctx, &new_ctx, NULL, &lmf, native_offset);
		g_assert (ji);

		ctx = new_ctx;
		
		if (MONO_CONTEXT_GET_BP (&ctx) >= jit_tls->end_of_stack)
			return FALSE;

	} while (skip-- > 0);

	*method = mono_method_get_object (domain, ji->method, NULL);
	*iloffset = mono_debug_il_offset_from_address (ji->method, *native_offset);

	if (need_file_info) {
		gchar *filename;

		filename = mono_debug_source_location_from_address (ji->method, *native_offset, line);

		*file = mono_string_new (domain, filename ? filename : "<unknown>");
		*column = 0;

		g_free (filename);
	}

	return TRUE;
}

/**
 * arch_handle_exception:
 * @ctx: saved processor state
 * @obj: the exception object
 * @test_only: only test if the exception is caught, but dont call handlers
 *
 *
 */
gboolean
arch_handle_exception (MonoContext *ctx, gpointer obj, gboolean test_only)
{
	MonoDomain *domain = mono_domain_get ();
	MonoJitInfo *ji;
	static int (*call_filter) (MonoContext *, gpointer, gpointer) = NULL;
	MonoJitTlsData *jit_tls = TlsGetValue (mono_jit_tls_id);
	MonoLMF *lmf = jit_tls->lmf;		
	GList *trace_ips = NULL;

	g_assert (ctx != NULL);
	if (!obj) {
		MonoException *ex = mono_get_exception_null_reference ();
		ex->message = mono_string_new (domain, 
		        "Object reference not set to an instance of an object");
		obj = (MonoObject *)ex;
	} 

	g_assert (mono_object_isinst (obj, mono_defaults.exception_class));

	if (!call_filter)
		call_filter = arch_get_call_filter ();

	g_assert (jit_tls->end_of_stack);
	g_assert (jit_tls->abort_func);

	if (!test_only) {
		MonoContext ctx_cp = *ctx;
		if (!arch_handle_exception (&ctx_cp, obj, TRUE)) {
			if (mono_break_on_exc) {
				if (mono_debug_format != MONO_DEBUG_FORMAT_NONE)
					mono_debug_make_symbols ();
				G_BREAKPOINT ();
			}
			mono_unhandled_exception (obj);
		}
	}

	while (1) {
		MonoContext new_ctx;
		char *trace = NULL;
		
		ji = mono_arch_find_jit_info (domain, jit_tls, ctx, &new_ctx, 
					      test_only ? &trace : NULL, &lmf, NULL);
		g_assert (ji);

		if (ji->method != mono_start_method) {
			
			if (test_only) {
				char *tmp, *strace;

				trace_ips = g_list_append (trace_ips, MONO_CONTEXT_GET_IP (ctx));

				if (!((MonoException*)obj)->stack_trace)
					strace = g_strdup ("");
				else
					strace = mono_string_to_utf8 (((MonoException*)obj)->stack_trace);

				tmp = g_strdup_printf ("%s%s\n", strace, trace);
				g_free (strace);

				((MonoException*)obj)->stack_trace = mono_string_new (domain, tmp);

				g_free (tmp);
			}

			if (ji->num_clauses) {
				int i;
				
				g_assert (ji->clauses);
			
				for (i = 0; i < ji->num_clauses; i++) {
					MonoJitExceptionInfo *ei = &ji->clauses [i];

					if (ei->try_start <= MONO_CONTEXT_GET_IP (ctx) && 
					    MONO_CONTEXT_GET_IP (ctx) <= ei->try_end) { 
						/* catch block */
						if ((ei->flags == MONO_EXCEPTION_CLAUSE_NONE && 
						     mono_object_isinst (obj, mono_class_get (ji->method->klass->image, ei->data.token))) ||
						    ((ei->flags == MONO_EXCEPTION_CLAUSE_FILTER &&
						      call_filter (ctx, ei->data.filter, obj)))) {
							if (test_only) {
								((MonoException*)obj)->trace_ips = glist_to_array (trace_ips);
								g_list_free (trace_ips);
								return TRUE;
							}
							MONO_CONTEXT_SET_IP (ctx, ei->handler_start);
							MONO_CONTEXT_SET_EXCREG (ctx, obj);
							jit_tls->lmf = lmf;
							return 0;
						}
					}
				}

				/* no handler found - we need to call all finally handlers */
				if (!test_only) {
					for (i = 0; i < ji->num_clauses; i++) {
						MonoJitExceptionInfo *ei = &ji->clauses [i];
						
						if (ei->try_start <= MONO_CONTEXT_GET_IP (ctx) && 
						    MONO_CONTEXT_GET_IP (ctx) < ei->try_end &&
						    (ei->flags & MONO_EXCEPTION_CLAUSE_FINALLY)) {
							call_filter (ctx, ei->handler_start, NULL);
						}
					}
				}
			}
		}

		g_free (trace);
			
		*ctx = new_ctx;

		if (ji->method == mono_start_method || MONO_CONTEXT_GET_BP (ctx) >= jit_tls->end_of_stack) {
			if (!test_only) {
				jit_tls->lmf = lmf;
				jit_tls->abort_func (obj);
				g_assert_not_reached ();
			} else {
				((MonoException*)obj)->trace_ips = glist_to_array (trace_ips);
				g_list_free (trace_ips);
				return FALSE;
			}
		}
	}

	g_assert_not_reached ();
}

