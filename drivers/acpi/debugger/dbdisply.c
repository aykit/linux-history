/*******************************************************************************
 *
 * Module Name: dbdisply - debug display commands
 *              $Revision: 78 $
 *
 ******************************************************************************/

/*
 *  Copyright (C) 2000 - 2002, R. Byron Moore
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */


#include "acpi.h"
#include "amlcode.h"
#include "acdispat.h"
#include "acnamesp.h"
#include "acparser.h"
#include "acinterp.h"
#include "acdebug.h"


#ifdef ACPI_DEBUGGER


#define _COMPONENT          ACPI_CA_DEBUGGER
	 ACPI_MODULE_NAME    ("dbdisply")


/******************************************************************************
 *
 * FUNCTION:    Acpi_db_get_pointer
 *
 * PARAMETERS:  Target          - Pointer to string to be converted
 *
 * RETURN:      Converted pointer
 *
 * DESCRIPTION: Convert an ascii pointer value to a real value
 *
 *****************************************************************************/

void *
acpi_db_get_pointer (
	void                    *target)
{
	void                    *obj_ptr;


#if ACPI_MACHINE_WIDTH == 16
#include <stdio.h>

	/* Have to handle 16-bit pointers of the form segment:offset */

	if (!sscanf (target, "%p", &obj_ptr)) {
		acpi_os_printf ("Invalid pointer: %s\n", target);
		return (NULL);
	}

#else

	/* Simple flat pointer */

	obj_ptr = ACPI_TO_POINTER (ACPI_STRTOUL (target, NULL, 16));
#endif

	return (obj_ptr);
}


/*******************************************************************************
 *
 * FUNCTION:    Acpi_db_dump_parser_descriptor
 *
 * PARAMETERS:  Op              - A parser Op descriptor
 *
 * RETURN:      None
 *
 * DESCRIPTION: Display a formatted parser object
 *
 ******************************************************************************/

void
acpi_db_dump_parser_descriptor (
	acpi_parse_object       *op)
{
	const acpi_opcode_info  *info;


	info = acpi_ps_get_opcode_info (op->common.aml_opcode);

	acpi_os_printf ("Parser Op Descriptor:\n");
	acpi_os_printf ("%20.20s : %4.4X\n", "Opcode", op->common.aml_opcode);

	ACPI_DEBUG_ONLY_MEMBERS (acpi_os_printf ("%20.20s : %s\n", "Opcode Name", info->name));

	acpi_os_printf ("%20.20s : %p\n", "Value/Arg_list", op->common.value.arg);
	acpi_os_printf ("%20.20s : %p\n", "Parent", op->common.parent);
	acpi_os_printf ("%20.20s : %p\n", "Next_op", op->common.next);
}


/*******************************************************************************
 *
 * FUNCTION:    Acpi_db_decode_and_display_object
 *
 * PARAMETERS:  Target          - String with object to be displayed.  Names
 *                                and hex pointers are supported.
 *              Output_type     - Byte, Word, Dword, or Qword (B|W|D|Q)
 *
 * RETURN:      None
 *
 * DESCRIPTION: Display a formatted ACPI object
 *
 ******************************************************************************/

void
acpi_db_decode_and_display_object (
	NATIVE_CHAR             *target,
	NATIVE_CHAR             *output_type)
{
	void                    *obj_ptr;
	acpi_namespace_node     *node;
	acpi_operand_object     *obj_desc;
	u32                     display = DB_BYTE_DISPLAY;
	NATIVE_CHAR             buffer[80];
	acpi_buffer             ret_buf;
	acpi_status             status;
	u32                     size;


	if (!target) {
		return;
	}

	/* Decode the output type */

	if (output_type) {
		ACPI_STRUPR (output_type);
		if (output_type[0] == 'W') {
			display = DB_WORD_DISPLAY;
		}
		else if (output_type[0] == 'D') {
			display = DB_DWORD_DISPLAY;
		}
		else if (output_type[0] == 'Q') {
			display = DB_QWORD_DISPLAY;
		}
	}

	ret_buf.length = sizeof (buffer);
	ret_buf.pointer = buffer;

	/* Differentiate between a number and a name */

	if ((target[0] >= 0x30) && (target[0] <= 0x39)) {
		obj_ptr = acpi_db_get_pointer (target);
		if (!acpi_os_readable (obj_ptr, 16)) {
			acpi_os_printf ("Address %p is invalid in this address space\n", obj_ptr);
			return;
		}

		/* Decode the object type */

		switch (ACPI_GET_DESCRIPTOR_TYPE (obj_ptr)) {
		case ACPI_DESC_TYPE_NAMED:

			/* This is a namespace Node */

			if (!acpi_os_readable (obj_ptr, sizeof (acpi_namespace_node))) {
				acpi_os_printf ("Cannot read entire Named object at address %p\n", obj_ptr);
				return;
			}

			node = obj_ptr;
			goto dump_nte;


		case ACPI_DESC_TYPE_OPERAND:

			/* This is a ACPI OPERAND OBJECT */

			if (!acpi_os_readable (obj_ptr, sizeof (acpi_operand_object))) {
				acpi_os_printf ("Cannot read entire ACPI object at address %p\n", obj_ptr);
				return;
			}

			acpi_ut_dump_buffer (obj_ptr, sizeof (acpi_operand_object), display, ACPI_UINT32_MAX);
			acpi_ex_dump_object_descriptor (obj_ptr, 1);
			break;


		case ACPI_DESC_TYPE_PARSER:

			/* This is a Parser Op object */

			if (!acpi_os_readable (obj_ptr, sizeof (acpi_parse_object))) {
				acpi_os_printf ("Cannot read entire Parser object at address %p\n", obj_ptr);
				return;
			}

			acpi_ut_dump_buffer (obj_ptr, sizeof (acpi_parse_object), display, ACPI_UINT32_MAX);
			acpi_db_dump_parser_descriptor ((acpi_parse_object *) obj_ptr);
			break;


		default:

			/* Is not a recognizeable object */

			size = 16;
			if (acpi_os_readable (obj_ptr, 64)) {
				size = 64;
			}

			/* Just dump some memory */

			acpi_ut_dump_buffer (obj_ptr, size, display, ACPI_UINT32_MAX);
			break;
		}

		return;
	}

	/* The parameter is a name string that must be resolved to a Named obj */

	node = acpi_db_local_ns_lookup (target);
	if (!node) {
		return;
	}


dump_nte:
	/* Now dump the Named obj */

	status = acpi_get_name (node, ACPI_FULL_PATHNAME, &ret_buf);
	if (ACPI_FAILURE (status)) {
		acpi_os_printf ("Could not convert name to pathname\n");
	}

	else {
		acpi_os_printf ("Object (%p) Pathname: %s\n", node, ret_buf.pointer);
	}

	if (!acpi_os_readable (node, sizeof (acpi_namespace_node))) {
		acpi_os_printf ("Invalid Named object at address %p\n", node);
		return;
	}

	acpi_ut_dump_buffer ((void *) node, sizeof (acpi_namespace_node), display, ACPI_UINT32_MAX);
	acpi_ex_dump_node (node, 1);

	obj_desc = acpi_ns_get_attached_object (node);
	if (obj_desc) {
		acpi_os_printf ("\nAttached Object (%p):\n", obj_desc);
		if (!acpi_os_readable (obj_desc, sizeof (acpi_operand_object))) {
			acpi_os_printf ("Invalid internal ACPI Object at address %p\n", obj_desc);
			return;
		}

		acpi_ut_dump_buffer ((void *) obj_desc, sizeof (acpi_operand_object), display, ACPI_UINT32_MAX);
		acpi_ex_dump_object_descriptor (obj_desc, 1);
	}
}


/*******************************************************************************
 *
 * FUNCTION:    Acpi_db_decode_internal_object
 *
 * PARAMETERS:  Obj_desc        - Object to be displayed
 *
 * RETURN:      None
 *
 * DESCRIPTION: Short display of an internal object.  Numbers and Strings.
 *
 ******************************************************************************/

void
acpi_db_decode_internal_object (
	acpi_operand_object     *obj_desc)
{
	u32                     i;


	if (!obj_desc) {
		acpi_os_printf (" Uninitialized\n");
		return;
	}

	if (ACPI_GET_DESCRIPTOR_TYPE (obj_desc) != ACPI_DESC_TYPE_OPERAND) {
		acpi_os_printf ("%p", obj_desc);
		return;
	}

	acpi_os_printf (" %s", acpi_ut_get_object_type_name (obj_desc));

	switch (ACPI_GET_OBJECT_TYPE (obj_desc)) {
	case ACPI_TYPE_INTEGER:

		acpi_os_printf (" %8.8X%8.8X", ACPI_HIDWORD (obj_desc->integer.value),
				   ACPI_LODWORD (obj_desc->integer.value));
		break;


	case ACPI_TYPE_STRING:

		acpi_os_printf ("(%d) \"%.24s",
				obj_desc->string.length, obj_desc->string.pointer);

		if (obj_desc->string.length > 24)
		{
			acpi_os_printf ("...");
		}
		else
		{
			acpi_os_printf ("\"");
		}
		break;


	case ACPI_TYPE_BUFFER:

		acpi_os_printf ("(%d)", obj_desc->buffer.length);
		for (i = 0; (i < 8) && (i < obj_desc->buffer.length); i++) {
			acpi_os_printf (" %2.2X", obj_desc->buffer.pointer[i]);
		}
		break;


	default:

		acpi_os_printf ("%p", obj_desc);
		break;
	}
}


/*******************************************************************************
 *
 * FUNCTION:    Acpi_db_decode_node
 *
 * PARAMETERS:  Node        - Object to be displayed
 *
 * RETURN:      None
 *
 * DESCRIPTION: Short display of a namespace node
 *
 ******************************************************************************/

void
acpi_db_decode_node (
	acpi_namespace_node     *node)
{


	acpi_os_printf ("<Node>          Name %4.4s Type-%s",
		node->name.ascii, acpi_ut_get_type_name (node->type));

	if (node->flags & ANOBJ_METHOD_ARG) {
		acpi_os_printf (" [Method Arg]");
	}
	if (node->flags & ANOBJ_METHOD_LOCAL) {
		acpi_os_printf (" [Method Local]");
	}

	acpi_db_decode_internal_object (acpi_ns_get_attached_object (node));
}


/*******************************************************************************
 *
 * FUNCTION:    Acpi_db_display_internal_object
 *
 * PARAMETERS:  Obj_desc        - Object to be displayed
 *              Walk_state      - Current walk state
 *
 * RETURN:      None
 *
 * DESCRIPTION: Short display of an internal object
 *
 ******************************************************************************/

void
acpi_db_display_internal_object (
	acpi_operand_object     *obj_desc,
	acpi_walk_state         *walk_state)
{
	u8                      type;


	acpi_os_printf ("%p ", obj_desc);

	if (!obj_desc) {
		acpi_os_printf ("<Null_obj>\n");
		return;
	}

	/* Decode the object type */

	switch (ACPI_GET_DESCRIPTOR_TYPE (obj_desc)) {
	case ACPI_DESC_TYPE_PARSER:

		acpi_os_printf ("<Parser> ");
		break;


	case ACPI_DESC_TYPE_NAMED:

		acpi_db_decode_node ((acpi_namespace_node *) obj_desc);
		break;


	case ACPI_DESC_TYPE_OPERAND:

		type = ACPI_GET_OBJECT_TYPE (obj_desc);
		if (type > INTERNAL_TYPE_MAX) {
			acpi_os_printf (" Type %hX [Invalid Type]", type);
			return;
		}

		/* Decode the ACPI object type */

		switch (ACPI_GET_OBJECT_TYPE (obj_desc)) {
		case INTERNAL_TYPE_REFERENCE:

			switch (obj_desc->reference.opcode) {
			case AML_LOCAL_OP:

				acpi_os_printf ("[Local%d] ", obj_desc->reference.offset);
				if (walk_state) {
					obj_desc = walk_state->local_variables[obj_desc->reference.offset].object;
					acpi_os_printf ("%p", obj_desc);
					acpi_db_decode_internal_object (obj_desc);
				}
				break;


			case AML_ARG_OP:

				acpi_os_printf ("[Arg%d] ", obj_desc->reference.offset);
				if (walk_state) {
					obj_desc = walk_state->arguments[obj_desc->reference.offset].object;
					acpi_os_printf ("%p", obj_desc);
					acpi_db_decode_internal_object (obj_desc);
				}
				break;


			case AML_DEBUG_OP:

				acpi_os_printf ("[Debug] ");
				break;


			case AML_INDEX_OP:

				acpi_os_printf ("[Index]        ");
				acpi_db_decode_internal_object (obj_desc->reference.object);
				break;


			case AML_REF_OF_OP:

				acpi_os_printf ("[Reference]    ");

				/* Reference can be to a Node or an Operand object */

				switch (ACPI_GET_DESCRIPTOR_TYPE (obj_desc->reference.object)) {
				case ACPI_DESC_TYPE_NAMED:
					acpi_db_decode_node (obj_desc->reference.object);
					break;

				case ACPI_DESC_TYPE_OPERAND:
					acpi_db_decode_internal_object (obj_desc->reference.object);
					break;

				default:
					break;
				}
				break;


			default:

				acpi_os_printf ("Unknown Reference opcode %X\n",
					obj_desc->reference.opcode);
				break;
			}
			break;

		default:

			acpi_os_printf ("<Obj> ");
			acpi_os_printf ("         ");
			acpi_db_decode_internal_object (obj_desc);
			break;
		}
		break;


	default:

		acpi_os_printf ("<Not a valid ACPI Object Descriptor> ");
		break;
	}

	acpi_os_printf ("\n");
}


/*******************************************************************************
 *
 * FUNCTION:    Acpi_db_display_method_info
 *
 * PARAMETERS:  Start_op        - Root of the control method parse tree
 *
 * RETURN:      None
 *
 * DESCRIPTION: Display information about the current method
 *
 ******************************************************************************/

void
acpi_db_display_method_info (
	acpi_parse_object       *start_op)
{
	acpi_walk_state         *walk_state;
	acpi_operand_object     *obj_desc;
	acpi_namespace_node     *node;
	acpi_parse_object       *root_op;
	acpi_parse_object       *op;
	const acpi_opcode_info  *op_info;
	u32                     num_ops = 0;
	u32                     num_operands = 0;
	u32                     num_operators = 0;
	u32                     num_remaining_ops = 0;
	u32                     num_remaining_operands = 0;
	u32                     num_remaining_operators = 0;
	u32                     num_args;
	u32                     concurrency;
	u8                      count_remaining = FALSE;


	walk_state = acpi_ds_get_current_walk_state (acpi_gbl_current_walk_list);
	if (!walk_state) {
		acpi_os_printf ("There is no method currently executing\n");
		return;
	}

	obj_desc = walk_state->method_desc;
	node    = walk_state->method_node;

	num_args    = obj_desc->method.param_count;
	concurrency = obj_desc->method.concurrency;

	acpi_os_printf ("Currently executing control method is [%4.4s]\n", node->name.ascii);
	acpi_os_printf ("%X arguments, max concurrency = %X\n", num_args, concurrency);


	root_op = start_op;
	while (root_op->common.parent) {
		root_op = root_op->common.parent;
	}

	op = root_op;

	while (op) {
		if (op == start_op) {
			count_remaining = TRUE;
		}

		num_ops++;
		if (count_remaining) {
			num_remaining_ops++;
		}

		/* Decode the opcode */

		op_info = acpi_ps_get_opcode_info (op->common.aml_opcode);
		switch (op_info->class) {
		case AML_CLASS_ARGUMENT:
			if (count_remaining) {
				num_remaining_operands++;
			}

			num_operands++;
			break;

		case AML_CLASS_UNKNOWN:
			/* Bad opcode or ASCII character */

			continue;

		default:
			if (count_remaining) {
				num_remaining_operators++;
			}

			num_operators++;
			break;
		}

		op = acpi_ps_get_depth_next (start_op, op);
	}

	acpi_os_printf ("Method contains:     %X AML Opcodes - %X Operators, %X Operands\n",
			 num_ops, num_operators, num_operands);

	acpi_os_printf ("Remaining to execute: %X AML Opcodes - %X Operators, %X Operands\n",
			 num_remaining_ops, num_remaining_operators, num_remaining_operands);
}


/*******************************************************************************
 *
 * FUNCTION:    Acpi_db_display_locals
 *
 * PARAMETERS:  None
 *
 * RETURN:      None
 *
 * DESCRIPTION: Display all locals for the currently running control method
 *
 ******************************************************************************/

void
acpi_db_display_locals (void)
{
	u32                     i;
	acpi_walk_state         *walk_state;
	acpi_operand_object     *obj_desc;
	acpi_namespace_node     *node;


	walk_state = acpi_ds_get_current_walk_state (acpi_gbl_current_walk_list);
	if (!walk_state) {
		acpi_os_printf ("There is no method currently executing\n");
		return;
	}

	obj_desc = walk_state->method_desc;
	node = walk_state->method_node;
	acpi_os_printf ("Local Variables for method [%4.4s]:\n", node->name.ascii);

	for (i = 0; i < MTH_NUM_LOCALS; i++) {
		obj_desc = walk_state->local_variables[i].object;
		acpi_os_printf ("Local%d: ", i);
		acpi_db_display_internal_object (obj_desc, walk_state);
	}
}


/*******************************************************************************
 *
 * FUNCTION:    Acpi_db_display_arguments
 *
 * PARAMETERS:  None
 *
 * RETURN:      None
 *
 * DESCRIPTION: Display all arguments for the currently running control method
 *
 ******************************************************************************/

void
acpi_db_display_arguments (void)
{
	u32                     i;
	acpi_walk_state         *walk_state;
	acpi_operand_object     *obj_desc;
	u32                     num_args;
	u32                     concurrency;
	acpi_namespace_node     *node;


	walk_state = acpi_ds_get_current_walk_state (acpi_gbl_current_walk_list);
	if (!walk_state) {
		acpi_os_printf ("There is no method currently executing\n");
		return;
	}

	obj_desc = walk_state->method_desc;
	node    = walk_state->method_node;

	num_args    = obj_desc->method.param_count;
	concurrency = obj_desc->method.concurrency;

	acpi_os_printf ("Method [%4.4s] has %X arguments, max concurrency = %X\n",
			node->name.ascii, num_args, concurrency);

	for (i = 0; i < num_args; i++) {
		obj_desc = walk_state->arguments[i].object;
		acpi_os_printf ("Arg%d: ", i);
		acpi_db_display_internal_object (obj_desc, walk_state);
	}
}


/*******************************************************************************
 *
 * FUNCTION:    Acpi_db_display_results
 *
 * PARAMETERS:  None
 *
 * RETURN:      None
 *
 * DESCRIPTION: Display current contents of a method result stack
 *
 ******************************************************************************/

void
acpi_db_display_results (void)
{
	u32                     i;
	acpi_walk_state         *walk_state;
	acpi_operand_object     *obj_desc;
	u32                     num_results = 0;
	acpi_namespace_node     *node;


	walk_state = acpi_ds_get_current_walk_state (acpi_gbl_current_walk_list);
	if (!walk_state) {
		acpi_os_printf ("There is no method currently executing\n");
		return;
	}

	obj_desc = walk_state->method_desc;
	node = walk_state->method_node;

	if (walk_state->results) {
		num_results = walk_state->results->results.num_results;
	}

	acpi_os_printf ("Method [%4.4s] has %X stacked result objects\n",
		node->name.ascii, num_results);

	for (i = 0; i < num_results; i++) {
		obj_desc = walk_state->results->results.obj_desc[i];
		acpi_os_printf ("Result%d: ", i);
		acpi_db_display_internal_object (obj_desc, walk_state);
	}
}


/*******************************************************************************
 *
 * FUNCTION:    Acpi_db_display_calling_tree
 *
 * PARAMETERS:  None
 *
 * RETURN:      None
 *
 * DESCRIPTION: Display current calling tree of nested control methods
 *
 ******************************************************************************/

void
acpi_db_display_calling_tree (void)
{
	acpi_walk_state         *walk_state;
	acpi_namespace_node     *node;


	walk_state = acpi_ds_get_current_walk_state (acpi_gbl_current_walk_list);
	if (!walk_state) {
		acpi_os_printf ("There is no method currently executing\n");
		return;
	}

	node = walk_state->method_node;
	acpi_os_printf ("Current Control Method Call Tree\n");

	while (walk_state) {
		node = walk_state->method_node;

		acpi_os_printf ("  [%4.4s]\n", node->name.ascii);

		walk_state = walk_state->next;
	}
}


/*******************************************************************************
 *
 * FUNCTION:    Acpi_db_display_result_object
 *
 * PARAMETERS:  Obj_desc        - Object to be displayed
 *              Walk_state      - Current walk state
 *
 * RETURN:      None
 *
 * DESCRIPTION: Display the result of an AML opcode
 *
 * Note: Curently only displays the result object if we are single stepping.
 * However, this output may be useful in other contexts and could be enabled
 * to do so if needed.
 *
 ******************************************************************************/

void
acpi_db_display_result_object (
	acpi_operand_object     *obj_desc,
	acpi_walk_state         *walk_state)
{

	/* Only display if single stepping */

	if (!acpi_gbl_cm_single_step) {
		return;
	}

	acpi_os_printf ("Result_obj: ");
	acpi_db_display_internal_object (obj_desc, walk_state);
	acpi_os_printf ("\n");
}


/*******************************************************************************
 *
 * FUNCTION:    Acpi_db_display_argument_object
 *
 * PARAMETERS:  Obj_desc        - Object to be displayed
 *              Walk_state      - Current walk state
 *
 * RETURN:      None
 *
 * DESCRIPTION: Display the result of an AML opcode
 *
 ******************************************************************************/

void
acpi_db_display_argument_object (
	acpi_operand_object     *obj_desc,
	acpi_walk_state         *walk_state)
{

	if (!acpi_gbl_cm_single_step) {
		return;
	}

	acpi_os_printf ("Arg_obj: ");
	acpi_db_display_internal_object (obj_desc, walk_state);
}

#endif /* ACPI_DEBUGGER */

