/*
 *Macro calls to generate WTP indications and confirmations. See documentation
 *for guidance how to use and update these.
 *
 *Note that the address five-tuple and tid are fields of wtp machine.
 * 
 *By Aarno Syv�nen for WapIt Ltd.
 */

WSP_EVENT(TRInvokeIndication,
          {
          INTEGER(ack_type);
          OCTSTR(user_data);
          INTEGER(tcl);
          MACHINE(machine);
	  })

WSP_EVENT(TRInvokeConfirmation,
	  {
          OCTSTR(exit_info);
          INTEGER(exit_info_present);
          MACHINE(machine);
          })

WSP_EVENT(TRAbortIndication,
          {
          INTEGER(abort_code);
          MACHINE(machine);
          })

#undef WSP_EVENT
#undef OCTSTR
#undef INTEGER
#undef MACHINE






