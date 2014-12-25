#include <memory>
#include <memory.h>
#include "base_target_client.h"
#include "disassembler.h"
#include "dis_client.h"

bool
base_target_client::check_for_back_edge (disassembler *dis, char *start,
                                         char *hook_end, char *code_end)
{
  std::auto_ptr<dis_client> backedge_check_client (
      new_backedge_check_client (reinterpret_cast<intptr_t> (start),
                                 reinterpret_cast<intptr_t> (hook_end)));
  dis->set_client (backedge_check_client.get ());
  for (char *i = hook_end;
       i < code_end && backedge_check_client->is_accept ();)
    {
      int len = dis->instruction_decode (i);
      i += len;
    }
  return backedge_check_client->is_accept ();
}

target_client::check_code_status
base_target_client::check_code (void *code_point, const char *name,
                                int code_size, code_manager *m,
                                code_context **ppcontext)
{
  std::auto_ptr<dis_client> code_check_client (new_code_check_client ());
  std::auto_ptr<disassembler> dis (new_disassembler ());
  dis->set_client (code_check_client.get ());
  int _byte_needed_to_modify = byte_needed_to_modify ();
  char *start = static_cast<char *> (code_point);
  int current = 0;
  if (code_size < _byte_needed_to_modify)
    return check_code_too_small;
  while (current < _byte_needed_to_modify && code_check_client->is_accept ())
    {
      int len = dis->instruction_decode (start);
      current += len;
      start += len;
    }
  if (code_check_client->is_accept () == false)
    {
      return check_code_not_accept;
    }
  if (current > max_tempoline_insert_space ())
    {
      return check_code_exceed_trampoline;
    }
  if (!check_for_back_edge (dis.get (), static_cast<char *> (code_point),
                            start,
                            static_cast<char *> (code_point) + code_size))
    {
      return check_code_back_edge;
    }
  code_context *context;
  context = m->new_context (name);
  if (context == NULL)
    return check_code_memory;
  context->code_point = code_point;
  context->machine_defined = reinterpret_cast<void *> (current);
  *ppcontext = context;
  return check_code_okay;
}

bool
base_target_client::build_trampoline (code_manager *m, code_context *context,
                                      pfn_called_callback called_callback,
                                      pfn_ret_callback return_callback)
{
  char *const _template_start = template_start ();
  char *const _template_ret_start = template_ret_start ();
  char *const _template_end = template_end ();
  const int template_code_size = (char *)_template_end
                                 - (char *)_template_start;
  const int template_size = template_code_size + sizeof (intptr_t) * 4;
  void *code_mem = m->new_code_mem (context->code_point, template_size);
  if (!code_mem)
    {
      return false;
    }
  // check if we can jump to our code.
  intptr_t code_mem_int = reinterpret_cast<intptr_t> (code_mem);
  intptr_t code_start = code_mem_int + sizeof (intptr_t) * 4;
  const intptr_t target_code_point
      = reinterpret_cast<intptr_t> (context->code_point);
  // FIXME: need to delete code mem before returns
  if (!check_jump_dist (target_code_point, code_start))
    return false;

  context->trampoline_code_start = reinterpret_cast<char *> (code_start);
  context->trampoline_code_end = reinterpret_cast<char *> (code_start)
                                 + template_code_size;
  // copy the hook template to code mem.
  memcpy (reinterpret_cast<void *> (code_start), (char *)_template_start,
          template_code_size);
  // copy the original target code to trampoline
  char *copy_start = reinterpret_cast<char *> (code_start)
                     + (_template_ret_start - _template_start)
                     - max_tempoline_insert_space ();

  int code_len = reinterpret_cast<intptr_t> (context->machine_defined);
  memcpy (copy_start, context->code_point, code_len);
  context->called_callback = called_callback;
  context->return_callback = return_callback;
  const char *function_name = context->function_name;
  const void **modify_pointer
      = static_cast<const void **> (context->trampoline_code_start);
  int modified_code_len = code_len;
  modify_pointer[-4] = function_name;
  modify_pointer[-3] = (void *)called_callback;
  modify_pointer[-2]
      = reinterpret_cast<void *> (target_code_point + modified_code_len);
  modify_pointer[-1] = (void *)return_callback;
  flush_code (code_mem, template_size);
  return true;
}