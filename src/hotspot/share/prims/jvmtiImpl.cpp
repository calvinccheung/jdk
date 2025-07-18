/*
 * Copyright (c) 2003, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "classfile/javaClasses.hpp"
#include "classfile/symbolTable.hpp"
#include "code/nmethod.hpp"
#include "interpreter/interpreter.hpp"
#include "interpreter/oopMapCache.hpp"
#include "jvmtifiles/jvmtiEnv.hpp"
#include "logging/log.hpp"
#include "logging/logStream.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/resourceArea.hpp"
#include "oops/instanceKlass.hpp"
#include "oops/klass.inline.hpp"
#include "oops/oop.inline.hpp"
#include "oops/oopHandle.inline.hpp"
#include "prims/jvmtiAgentThread.hpp"
#include "prims/jvmtiEventController.inline.hpp"
#include "prims/jvmtiImpl.hpp"
#include "prims/jvmtiRedefineClasses.hpp"
#include "runtime/atomic.hpp"
#include "runtime/continuation.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/interfaceSupport.inline.hpp"
#include "runtime/javaCalls.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/os.hpp"
#include "runtime/serviceThread.hpp"
#include "runtime/signature.hpp"
#include "runtime/threadSMR.hpp"
#include "runtime/vframe.inline.hpp"
#include "runtime/vframe_hp.hpp"
#include "runtime/vmOperations.hpp"
#include "utilities/exceptions.hpp"

//
// class JvmtiAgentThread
//
// JavaThread used to wrap a thread started by an agent
// using the JVMTI method RunAgentThread.
//

JvmtiAgentThread::JvmtiAgentThread(JvmtiEnv* env, jvmtiStartFunction start_fn, const void *start_arg)
    : JavaThread(start_function_wrapper) {
    _env = env;
    _start_fn = start_fn;
    _start_arg = start_arg;
}

void
JvmtiAgentThread::start_function_wrapper(JavaThread *thread, TRAPS) {
    // It is expected that any Agent threads will be created as
    // Java Threads.  If this is the case, notification of the creation
    // of the thread is given in JavaThread::thread_main().
    assert(thread == JavaThread::current(), "sanity check");

    JvmtiAgentThread *dthread = (JvmtiAgentThread *)thread;
    dthread->call_start_function();
}

void
JvmtiAgentThread::call_start_function() {
    ThreadToNativeFromVM transition(this);
    _start_fn(_env->jvmti_external(), jni_environment(), (void*)_start_arg);
}

//
// class JvmtiBreakpoint
//

JvmtiBreakpoint::JvmtiBreakpoint(Method* m_method, jlocation location)
    : _method(m_method), _bci((int)location) {
  assert(_method != nullptr, "No method for breakpoint.");
  assert(_bci >= 0, "Negative bci for breakpoint.");
  oop class_holder_oop = _method->method_holder()->klass_holder();
  _class_holder = OopHandle(JvmtiExport::jvmti_oop_storage(), class_holder_oop);
}

JvmtiBreakpoint::JvmtiBreakpoint(const JvmtiBreakpoint& bp)
    : _method(bp._method), _bci(bp._bci) {
  _class_holder = OopHandle(JvmtiExport::jvmti_oop_storage(), bp._class_holder.resolve());
}

JvmtiBreakpoint::~JvmtiBreakpoint() {
  _class_holder.release(JvmtiExport::jvmti_oop_storage());
}

bool JvmtiBreakpoint::equals(const JvmtiBreakpoint& bp) const {
  return _method   == bp._method
    &&   _bci      == bp._bci;
}

address JvmtiBreakpoint::getBcp() const {
  return _method->bcp_from(_bci);
}

void JvmtiBreakpoint::each_method_version_do(method_action meth_act) {
  assert(!_method->is_old(), "the breakpoint method shouldn't be old");
  ((Method*)_method->*meth_act)(_bci);

  // add/remove breakpoint to/from versions of the method that are EMCP.
  Thread *thread = Thread::current();
  InstanceKlass* ik = _method->method_holder();
  Symbol* m_name = _method->name();
  Symbol* m_signature = _method->signature();

  // search previous versions if they exist
  for (InstanceKlass* pv_node = ik->previous_versions();
       pv_node != nullptr;
       pv_node = pv_node->previous_versions()) {
    Array<Method*>* methods = pv_node->methods();

    for (int i = methods->length() - 1; i >= 0; i--) {
      Method* method = methods->at(i);
      // Only set breakpoints in EMCP methods.
      // EMCP methods are old but not obsolete. Equivalent
      // Modulo Constant Pool means the method is equivalent except
      // the constant pool and instructions that access the constant
      // pool might be different.
      // If a breakpoint is set in a redefined method, its EMCP methods
      // must have a breakpoint also.
      // None of the methods are deleted until none are running.
      // This code could set a breakpoint in a method that
      // is never reached, but this won't be noticeable to the programmer.
      if (!method->is_obsolete() &&
          method->name() == m_name &&
          method->signature() == m_signature) {
        ResourceMark rm;
        log_debug(redefine, class, breakpoint)
          ("%sing breakpoint in %s(%s)", meth_act == &Method::set_breakpoint ? "sett" : "clear",
           method->name()->as_C_string(), method->signature()->as_C_string());
        (method->*meth_act)(_bci);
        break;
      }
    }
  }
}

void JvmtiBreakpoint::set() {
  each_method_version_do(&Method::set_breakpoint);
}

void JvmtiBreakpoint::clear() {
  each_method_version_do(&Method::clear_breakpoint);
}

void JvmtiBreakpoint::print_on(outputStream* out) const {
#ifndef PRODUCT
  ResourceMark rm;
  const char *class_name  = (_method == nullptr) ? "null" : _method->klass_name()->as_C_string();
  const char *method_name = (_method == nullptr) ? "null" : _method->name()->as_C_string();
  out->print("Breakpoint(%s,%s,%d,%p)", class_name, method_name, _bci, getBcp());
#endif
}


//
// class VM_ChangeBreakpoints
//
// Modify the Breakpoints data structure at a safepoint
//
// The caller of VM_ChangeBreakpoints operation should ensure that
// _bp.method is preserved until VM_ChangeBreakpoints is processed.

void VM_ChangeBreakpoints::doit() {
  if (_bp->method()->is_old()) {
    // The bp->_method became old because VMOp with class redefinition happened for this class
    // after JvmtiBreakpoint was created but before JVM_ChangeBreakpoints started.
    // All class breakpoints are cleared during redefinition, so don't set/clear this breakpoint.
   return;
  }
  switch (_operation) {
  case SET_BREAKPOINT:
    _breakpoints->set_at_safepoint(*_bp);
    break;
  case CLEAR_BREAKPOINT:
    _breakpoints->clear_at_safepoint(*_bp);
    break;
  default:
    assert(false, "Unknown operation");
  }
}

//
// class JvmtiBreakpoints
//
// a JVMTI internal collection of JvmtiBreakpoint
//

JvmtiBreakpoints::JvmtiBreakpoints()
    : _elements(5, mtServiceability) {
}

JvmtiBreakpoints:: ~JvmtiBreakpoints() {}

void JvmtiBreakpoints::print() {
#ifndef PRODUCT
  LogTarget(Trace, jvmti) log;
  LogStream log_stream(log);

  int n = length();
  for (int i = 0; i < n; i++) {
    JvmtiBreakpoint& bp = at(i);
    log_stream.print("%d: ", i);
    bp.print_on(&log_stream);
    log_stream.cr();
  }
#endif
}


void JvmtiBreakpoints::set_at_safepoint(JvmtiBreakpoint& bp) {
  assert(SafepointSynchronize::is_at_safepoint(), "must be at safepoint");

  int i = find(bp);
  if (i == -1) {
    append(bp);
    bp.set();
  }
}

void JvmtiBreakpoints::clear_at_safepoint(JvmtiBreakpoint& bp) {
  assert(SafepointSynchronize::is_at_safepoint(), "must be at safepoint");

  int i = find(bp);
  if (i != -1) {
    remove(i);
    bp.clear();
  }
}

int JvmtiBreakpoints::set(JvmtiBreakpoint& bp) {
  if (find(bp) != -1) {
    return JVMTI_ERROR_DUPLICATE;
  }

  // Ensure that bp._method is not deallocated before VM_ChangeBreakpoints::doit().
  methodHandle mh(Thread::current(), bp.method());
  VM_ChangeBreakpoints set_breakpoint(VM_ChangeBreakpoints::SET_BREAKPOINT, &bp);
  VMThread::execute(&set_breakpoint);
  return JVMTI_ERROR_NONE;
}

int JvmtiBreakpoints::clear(JvmtiBreakpoint& bp) {
  if (find(bp) == -1) {
    return JVMTI_ERROR_NOT_FOUND;
  }

  // Ensure that bp._method is not deallocated before VM_ChangeBreakpoints::doit().
  methodHandle mh(Thread::current(), bp.method());
  VM_ChangeBreakpoints clear_breakpoint(VM_ChangeBreakpoints::CLEAR_BREAKPOINT, &bp);
  VMThread::execute(&clear_breakpoint);
  return JVMTI_ERROR_NONE;
}

void JvmtiBreakpoints::clearall_in_class_at_safepoint(Klass* klass) {
  assert(SafepointSynchronize::is_at_safepoint(), "must be at safepoint");

  // Go backwards because this removes entries that are freed.
  for (int i = length() - 1; i >= 0; i--) {
    JvmtiBreakpoint& bp = at(i);
    if (bp.method()->method_holder() == klass) {
      bp.clear();
      remove(i);
    }
  }
}

//
// class JvmtiCurrentBreakpoints
//

JvmtiBreakpoints *JvmtiCurrentBreakpoints::_jvmti_breakpoints  = nullptr;

JvmtiBreakpoints& JvmtiCurrentBreakpoints::get_jvmti_breakpoints() {
  if (_jvmti_breakpoints == nullptr) {
    JvmtiBreakpoints* breakpoints = new JvmtiBreakpoints();
    if (!Atomic::replace_if_null(&_jvmti_breakpoints, breakpoints)) {
      // already created concurently
      delete breakpoints;
    }
  }
  return (*_jvmti_breakpoints);
}


///////////////////////////////////////////////////////////////
//
// class VM_BaseGetOrSetLocal
//

const jvalue VM_BaseGetOrSetLocal::_DEFAULT_VALUE = {0L};
// Constructor for non-object getter

VM_BaseGetOrSetLocal::VM_BaseGetOrSetLocal(JavaThread* calling_thread, jint depth,
                                           jint index, BasicType type, jvalue value, bool set, bool self)
  : _calling_thread(calling_thread)
  , _depth(depth)
  , _index(index)
  , _type(type)
  , _value(value)
  , _jvf(nullptr)
  , _set(set)
  , _self(self)
  , _result(JVMTI_ERROR_NONE)
{
}

// Check that the klass is assignable to a type with the given signature.
// Another solution could be to use the function Klass::is_subtype_of(type).
// But the type class can be forced to load/initialize eagerly in such a case.
// This may cause unexpected consequences like CFLH or class-init JVMTI events.
// It is better to avoid such a behavior.
bool VM_BaseGetOrSetLocal::is_assignable(const char* ty_sign, Klass* klass, Thread* thread) {
  assert(ty_sign != nullptr, "type signature must not be null");
  assert(thread != nullptr, "thread must not be null");
  assert(klass != nullptr, "klass must not be null");

  int len = (int) strlen(ty_sign);
  if (ty_sign[0] == JVM_SIGNATURE_CLASS &&
      ty_sign[len-1] == JVM_SIGNATURE_ENDCLASS) { // Need pure class/interface name
    ty_sign++;
    len -= 2;
  }
  TempNewSymbol ty_sym = SymbolTable::new_symbol(ty_sign, len);
  if (klass->name() == ty_sym) {
    return true;
  }
  // Compare primary supers
  int super_depth = klass->super_depth();
  int idx;
  for (idx = 0; idx < super_depth; idx++) {
    if (klass->primary_super_of_depth(idx)->name() == ty_sym) {
      return true;
    }
  }
  // Compare secondary supers
  const Array<Klass*>* sec_supers = klass->secondary_supers();
  for (idx = 0; idx < sec_supers->length(); idx++) {
    if (((Klass*) sec_supers->at(idx))->name() == ty_sym) {
      return true;
    }
  }
  return false;
}

// Checks error conditions:
//   JVMTI_ERROR_INVALID_SLOT
//   JVMTI_ERROR_TYPE_MISMATCH
// Returns: 'true' - everything is Ok, 'false' - error code

bool VM_BaseGetOrSetLocal::check_slot_type_lvt(javaVFrame* jvf) {
  Method* method = jvf->method();
  if (!method->has_localvariable_table()) {
    // Just to check index boundaries.
    jint extra_slot = (_type == T_LONG || _type == T_DOUBLE) ? 1 : 0;
    if (_index < 0 || _index + extra_slot >= method->max_locals()) {
      _result = JVMTI_ERROR_INVALID_SLOT;
      return false;
    }
    return true;
  }

  jint num_entries = method->localvariable_table_length();
  if (num_entries == 0) {
    _result = JVMTI_ERROR_INVALID_SLOT;
    return false;       // There are no slots
  }
  int signature_idx = -1;
  int vf_bci = jvf->bci();
  LocalVariableTableElement* table = method->localvariable_table_start();
  for (int i = 0; i < num_entries; i++) {
    int start_bci = table[i].start_bci;
    int end_bci = start_bci + table[i].length;

    // Here we assume that locations of LVT entries
    // with the same slot number cannot be overlapped
    if (_index == (jint) table[i].slot && start_bci <= vf_bci && vf_bci <= end_bci) {
      signature_idx = (int) table[i].descriptor_cp_index;
      break;
    }
  }
  if (signature_idx == -1) {
    _result = JVMTI_ERROR_INVALID_SLOT;
    return false;       // Incorrect slot index
  }
  Symbol*   sign_sym  = method->constants()->symbol_at(signature_idx);
  BasicType slot_type = Signature::basic_type(sign_sym);

  switch (slot_type) {
  case T_BYTE:
  case T_SHORT:
  case T_CHAR:
  case T_BOOLEAN:
    slot_type = T_INT;
    break;
  case T_ARRAY:
    slot_type = T_OBJECT;
    break;
  default:
    break;
  };
  if (_type != slot_type) {
    _result = JVMTI_ERROR_TYPE_MISMATCH;
    return false;
  }

  jobject jobj = _value.l;
  if (_set && slot_type == T_OBJECT && jobj != nullptr) { // null reference is allowed
    // Check that the jobject class matches the return type signature.
    oop obj = JNIHandles::resolve_external_guard(jobj);
    NULL_CHECK(obj, (_result = JVMTI_ERROR_INVALID_OBJECT, false));
    Klass* ob_k = obj->klass();
    NULL_CHECK(ob_k, (_result = JVMTI_ERROR_INVALID_OBJECT, false));

    const char* signature = (const char *) sign_sym->as_utf8();
    if (!is_assignable(signature, ob_k, VMThread::vm_thread())) {
      _result = JVMTI_ERROR_TYPE_MISMATCH;
      return false;
    }
  }
  return true;
}

bool VM_BaseGetOrSetLocal::check_slot_type_no_lvt(javaVFrame* jvf) {
  Method* method = jvf->method();
  jint extra_slot = (_type == T_LONG || _type == T_DOUBLE) ? 1 : 0;

  if (_index < 0 || _index + extra_slot >= method->max_locals()) {
    _result = JVMTI_ERROR_INVALID_SLOT;
    return false;
  }
  StackValueCollection *locals = _jvf->locals();
  BasicType slot_type = locals->at(_index)->type();

  if (slot_type == T_CONFLICT) {
    _result = JVMTI_ERROR_INVALID_SLOT;
    return false;
  }
  if (extra_slot) {
    BasicType extra_slot_type = locals->at(_index + 1)->type();
    if (extra_slot_type != T_INT) {
      _result = JVMTI_ERROR_INVALID_SLOT;
      return false;
    }
  }
  if (_type != slot_type && (_type == T_OBJECT || slot_type != T_INT)) {
    _result = JVMTI_ERROR_TYPE_MISMATCH;
    return false;
  }
  return true;
}

static bool can_be_deoptimized(vframe* vf) {
  return (vf->is_compiled_frame() && vf->fr().can_be_deoptimized());
}

bool VM_GetOrSetLocal::doit_prologue() {
  if (!_eb.deoptimize_objects(_depth, _depth)) {
    // The target frame is affected by a reallocation failure.
    _result = JVMTI_ERROR_OUT_OF_MEMORY;
    return false;
  }

  return true;
}

void VM_BaseGetOrSetLocal::doit() {
  _jvf = get_java_vframe();
  if (_jvf == nullptr) {
    return;
  };

  frame fr = _jvf->fr();
  if (_set && _depth != 0 && Continuation::is_frame_in_continuation(_jvf->thread(), fr)) {
    _result = JVMTI_ERROR_OPAQUE_FRAME; // deferred locals are not fully supported in continuations
    return;
  }

  Method* method = _jvf->method();
  if (getting_receiver()) {
    if (method->is_static()) {
      _result = JVMTI_ERROR_INVALID_SLOT;
      return;
    }
  } else {
    if (method->is_native()) {
      _result = JVMTI_ERROR_OPAQUE_FRAME;
      return;
    }

    if (!check_slot_type_no_lvt(_jvf)) {
      return;
    }
    if (method->has_localvariable_table() &&
        !check_slot_type_lvt(_jvf)) {
      return;
    }
  }

  InterpreterOopMap oop_mask;
  _jvf->method()->mask_for(_jvf->bci(), &oop_mask);
  if (oop_mask.is_dead(_index)) {
    // The local can be invalid and uninitialized in the scope of current bci
    _result = JVMTI_ERROR_INVALID_SLOT;
    return;
  }
  if (_set) {
    if (fr.is_heap_frame()) { // we want this check after the check for JVMTI_ERROR_INVALID_SLOT
      assert(Continuation::is_frame_in_continuation(_jvf->thread(), fr), "sanity check");
      // If the topmost frame is a heap frame, then it hasn't been thawed. This can happen
      // if we are executing at a return barrier safepoint. The callee frame has been popped,
      // but the caller frame has not been thawed. We can't support a JVMTI SetLocal in the callee
      // frame at this point, because we aren't truly in the callee yet.
      // fr.is_heap_frame() is impossible if a continuation is at a single step or breakpoint.
      _result = JVMTI_ERROR_OPAQUE_FRAME; // deferred locals are not fully supported in continuations
      return;
    }

    // Force deoptimization of frame if compiled because it's
    // possible the compiler emitted some locals as constant values,
    // meaning they are not mutable.
    if (can_be_deoptimized(_jvf)) {
      // Continuation can't be unmounted at this point (it was checked/reported in get_java_vframe).
      if (Continuation::is_frame_in_continuation(_jvf->thread(), fr)) {
        _result = JVMTI_ERROR_OPAQUE_FRAME; // can't deoptimize for top continuation frame
        return;
      }

      // Schedule deoptimization so that eventually the local
      // update will be written to an interpreter frame.
      Deoptimization::deoptimize_frame(_jvf->thread(), _jvf->fr().id());

      // Now store a new value for the local which will be applied
      // once deoptimization occurs. Note however that while this
      // write is deferred until deoptimization actually happens
      // can vframe created after this point will have its locals
      // reflecting this update so as far as anyone can see the
      // write has already taken place.

      // If we are updating an oop then get the oop from the handle
      // since the handle will be long gone by the time the deopt
      // happens. The oop stored in the deferred local will be
      // gc'd on its own.
      if (_type == T_OBJECT) {
        _value.l = cast_from_oop<jobject>(JNIHandles::resolve_external_guard(_value.l));
      }
      // Re-read the vframe so we can see that it is deoptimized
      // [ Only need because of assert in update_local() ]
      _jvf = get_java_vframe();
      ((compiledVFrame*)_jvf)->update_local(_type, _index, _value);
      return;
    }
    StackValueCollection *locals = _jvf->locals();
    Thread* current_thread = VMThread::vm_thread();
    HandleMark hm(current_thread);

    switch (_type) {
      case T_INT:    locals->set_int_at   (_index, _value.i); break;
      case T_LONG:   locals->set_long_at  (_index, _value.j); break;
      case T_FLOAT:  locals->set_float_at (_index, _value.f); break;
      case T_DOUBLE: locals->set_double_at(_index, _value.d); break;
      case T_OBJECT: {
        Handle ob_h(current_thread, JNIHandles::resolve_external_guard(_value.l));
        locals->set_obj_at (_index, ob_h);
        break;
      }
      default: ShouldNotReachHere();
    }
    _jvf->set_locals(locals);
  } else {
    if (_jvf->method()->is_native() && _jvf->is_compiled_frame()) {
      assert(getting_receiver(), "Can only get here when getting receiver");
      oop receiver = _jvf->fr().get_native_receiver();
      _value.l = JNIHandles::make_local(_calling_thread, receiver);
    } else {
      StackValueCollection *locals = _jvf->locals();

      switch (_type) {
        case T_INT:    _value.i = locals->int_at   (_index);   break;
        case T_LONG:   _value.j = locals->long_at  (_index);   break;
        case T_FLOAT:  _value.f = locals->float_at (_index);   break;
        case T_DOUBLE: _value.d = locals->double_at(_index);   break;
        case T_OBJECT: {
          // Wrap the oop to be returned in a local JNI handle since
          // oops_do() no longer applies after doit() is finished.
          oop obj = locals->obj_at(_index)();
          _value.l = JNIHandles::make_local(_calling_thread, obj);
          break;
        }
        default: ShouldNotReachHere();
      }
    }
  }
}

bool VM_BaseGetOrSetLocal::allow_nested_vm_operations() const {
  return true; // May need to deoptimize
}


///////////////////////////////////////////////////////////////
//
// class VM_GetOrSetLocal
//

// Constructor for non-object getter
VM_GetOrSetLocal::VM_GetOrSetLocal(JavaThread* thread, jint depth, jint index, BasicType type, bool self)
  : VM_BaseGetOrSetLocal(nullptr, depth, index, type, _DEFAULT_VALUE, false, self),
    _thread(thread),
    _eb(false, nullptr, nullptr)
{
}

// Constructor for object or non-object setter
VM_GetOrSetLocal::VM_GetOrSetLocal(JavaThread* thread, jint depth, jint index, BasicType type, jvalue value, bool self)
  : VM_BaseGetOrSetLocal(nullptr, depth, index, type, value, true, self),
    _thread(thread),
    _eb(type == T_OBJECT, JavaThread::current(), thread)
{
}

// Constructor for object getter
VM_GetOrSetLocal::VM_GetOrSetLocal(JavaThread* thread, JavaThread* calling_thread, jint depth, int index, bool self)
  : VM_BaseGetOrSetLocal(calling_thread, depth, index, T_OBJECT, _DEFAULT_VALUE, false, self),
    _thread(thread),
    _eb(true, calling_thread, thread)
{
}

vframe *VM_GetOrSetLocal::get_vframe() {
  if (!_thread->has_last_Java_frame()) {
    return nullptr;
  }
  RegisterMap reg_map(_thread,
                      RegisterMap::UpdateMap::include,
                      RegisterMap::ProcessFrames::include,
                      RegisterMap::WalkContinuation::include);
  vframe *vf = JvmtiEnvBase::get_cthread_last_java_vframe(_thread, &reg_map);
  int d = 0;
  while ((vf != nullptr) && (d < _depth)) {
    vf = vf->java_sender();
    d++;
  }
  return vf;
}

javaVFrame *VM_GetOrSetLocal::get_java_vframe() {
  vframe* vf = get_vframe();
  if (!_self && !_thread->is_suspended() && !_thread->is_carrier_thread_suspended()) {
    _result = JVMTI_ERROR_THREAD_NOT_SUSPENDED;
    return nullptr;
  }
  if (vf == nullptr) {
    _result = JVMTI_ERROR_NO_MORE_FRAMES;
    return nullptr;
  }
  javaVFrame *jvf = (javaVFrame*)vf;

  if (!vf->is_java_frame()) {
    _result = JVMTI_ERROR_OPAQUE_FRAME;
    return nullptr;
  }
  return jvf;
}

VM_GetReceiver::VM_GetReceiver(
    JavaThread* thread, JavaThread* caller_thread, jint depth, bool self)
    : VM_GetOrSetLocal(thread, caller_thread, depth, 0, self) {}


///////////////////////////////////////////////////////////////
//
// class VM_VirtualThreadGetOrSetLocal
//

// Constructor for non-object getter
VM_VirtualThreadGetOrSetLocal::VM_VirtualThreadGetOrSetLocal(JvmtiEnv* env, Handle vthread_h, jint depth,
                                                             jint index, BasicType type, bool self)
  : VM_BaseGetOrSetLocal(nullptr, depth, index, type, _DEFAULT_VALUE, false, self)
{
  _env = env;
  _vthread_h = vthread_h;
}

// Constructor for object or non-object setter
VM_VirtualThreadGetOrSetLocal::VM_VirtualThreadGetOrSetLocal(JvmtiEnv* env, Handle vthread_h, jint depth,
                                                             jint index, BasicType type, jvalue value, bool self)
  : VM_BaseGetOrSetLocal(nullptr, depth, index, type, value, true, self)
{
  _env = env;
  _vthread_h = vthread_h;
}

// Constructor for object getter
VM_VirtualThreadGetOrSetLocal::VM_VirtualThreadGetOrSetLocal(JvmtiEnv* env, Handle vthread_h, JavaThread* calling_thread,
                                                             jint depth, int index, bool self)
  : VM_BaseGetOrSetLocal(calling_thread, depth, index, T_OBJECT, _DEFAULT_VALUE, false, self)
{
  _env = env;
  _vthread_h = vthread_h;
}

javaVFrame *VM_VirtualThreadGetOrSetLocal::get_java_vframe() {
  JavaThread* java_thread = JvmtiEnvBase::get_JavaThread_or_null(_vthread_h());
  bool is_cont_mounted = (java_thread != nullptr);

  if (!(_self || JvmtiVTSuspender::is_vthread_suspended(_vthread_h()))) {
    _result = JVMTI_ERROR_THREAD_NOT_SUSPENDED;
    return nullptr;
  }
  javaVFrame* jvf = JvmtiEnvBase::get_vthread_jvf(_vthread_h());

  int d = 0;
  while ((jvf != nullptr) && (d < _depth)) {
    jvf = jvf->java_sender();
    d++;
  }

  if (d < _depth || jvf == nullptr) {
    _result = JVMTI_ERROR_NO_MORE_FRAMES;
    return nullptr;
  }

  if ((_set && !is_cont_mounted) || !jvf->is_java_frame()) {
    _result = JVMTI_ERROR_OPAQUE_FRAME;
    return nullptr;
  }
  return jvf;
}

VM_VirtualThreadGetReceiver::VM_VirtualThreadGetReceiver(
    JvmtiEnv* env, Handle vthread_h, JavaThread* caller_thread, jint depth, bool self)
    : VM_VirtualThreadGetOrSetLocal(env, vthread_h, caller_thread, depth, 0, self) {}


JvmtiDeferredEvent JvmtiDeferredEvent::compiled_method_load_event(
    nmethod* nm) {
  JvmtiDeferredEvent event = JvmtiDeferredEvent(TYPE_COMPILED_METHOD_LOAD);
  event._event_data.compiled_method_load = nm;
  return event;
}

JvmtiDeferredEvent JvmtiDeferredEvent::compiled_method_unload_event(
    jmethodID id, const void* code) {
  JvmtiDeferredEvent event = JvmtiDeferredEvent(TYPE_COMPILED_METHOD_UNLOAD);
  event._event_data.compiled_method_unload.method_id = id;
  event._event_data.compiled_method_unload.code_begin = code;
  return event;
}

JvmtiDeferredEvent JvmtiDeferredEvent::dynamic_code_generated_event(
      const char* name, const void* code_begin, const void* code_end) {
  JvmtiDeferredEvent event = JvmtiDeferredEvent(TYPE_DYNAMIC_CODE_GENERATED);
  // Need to make a copy of the name since we don't know how long
  // the event poster will keep it around after we enqueue the
  // deferred event and return. strdup() failure is handled in
  // the post() routine below.
  event._event_data.dynamic_code_generated.name = os::strdup(name);
  event._event_data.dynamic_code_generated.code_begin = code_begin;
  event._event_data.dynamic_code_generated.code_end = code_end;
  return event;
}

JvmtiDeferredEvent JvmtiDeferredEvent::class_unload_event(const char* name) {
  JvmtiDeferredEvent event = JvmtiDeferredEvent(TYPE_CLASS_UNLOAD);
  // Need to make a copy of the name since we don't know how long
  // the event poster will keep it around after we enqueue the
  // deferred event and return. strdup() failure is handled in
  // the post() routine below.
  event._event_data.class_unload.name = os::strdup(name);
  return event;
}

void JvmtiDeferredEvent::post() {
  assert(Thread::current()->is_service_thread(),
         "Service thread must post enqueued events");
  switch(_type) {
    case TYPE_COMPILED_METHOD_LOAD: {
      nmethod* nm = _event_data.compiled_method_load;
      JvmtiExport::post_compiled_method_load(nm);
      break;
    }
    case TYPE_COMPILED_METHOD_UNLOAD: {
      JvmtiExport::post_compiled_method_unload(
        _event_data.compiled_method_unload.method_id,
        _event_data.compiled_method_unload.code_begin);
      break;
    }
    case TYPE_DYNAMIC_CODE_GENERATED: {
      JvmtiExport::post_dynamic_code_generated_internal(
        // if strdup failed give the event a default name
        (_event_data.dynamic_code_generated.name == nullptr)
          ? "unknown_code" : _event_data.dynamic_code_generated.name,
        _event_data.dynamic_code_generated.code_begin,
        _event_data.dynamic_code_generated.code_end);
      if (_event_data.dynamic_code_generated.name != nullptr) {
        // release our copy
        os::free((void *)_event_data.dynamic_code_generated.name);
      }
      break;
    }
    case TYPE_CLASS_UNLOAD: {
      JvmtiExport::post_class_unload_internal(
        // if strdup failed give the event a default name
        (_event_data.class_unload.name == nullptr)
          ? "unknown_class" : _event_data.class_unload.name);
      if (_event_data.class_unload.name != nullptr) {
        // release our copy
        os::free((void *)_event_data.class_unload.name);
      }
      break;
    }
    default:
      ShouldNotReachHere();
  }
}

void JvmtiDeferredEvent::post_compiled_method_load_event(JvmtiEnv* env) {
  assert(_type == TYPE_COMPILED_METHOD_LOAD, "only user of this method");
  nmethod* nm = _event_data.compiled_method_load;
  JvmtiExport::post_compiled_method_load(env, nm);
}

void JvmtiDeferredEvent::run_nmethod_entry_barriers() {
  if (_type == TYPE_COMPILED_METHOD_LOAD) {
    _event_data.compiled_method_load->run_nmethod_entry_barrier();
  }
}


// Keep the nmethod for compiled_method_load from being unloaded.
void JvmtiDeferredEvent::oops_do(OopClosure* f, NMethodClosure* cf) {
  if (cf != nullptr && _type == TYPE_COMPILED_METHOD_LOAD) {
    cf->do_nmethod(_event_data.compiled_method_load);
  }
}

// The GC calls this and marks the nmethods here on the stack so that
// they cannot be unloaded while in the queue.
void JvmtiDeferredEvent::nmethods_do(NMethodClosure* cf) {
  if (cf != nullptr && _type == TYPE_COMPILED_METHOD_LOAD) {
    cf->do_nmethod(_event_data.compiled_method_load);
  }
}


bool JvmtiDeferredEventQueue::has_events() {
  // We save the queued events before the live phase and post them when it starts.
  // This code could skip saving the events on the queue before the live
  // phase and ignore them, but this would change how we do things now.
  // Starting the service thread earlier causes this to be called before the live phase begins.
  // The events on the queue should all be posted after the live phase so this is an
  // ok check.  Before the live phase, DynamicCodeGenerated events are posted directly.
  // If we add other types of events to the deferred queue, this could get ugly.
  return JvmtiEnvBase::get_phase() == JVMTI_PHASE_LIVE  && _queue_head != nullptr;
}

void JvmtiDeferredEventQueue::enqueue(JvmtiDeferredEvent event) {
  // Events get added to the end of the queue (and are pulled off the front).
  QueueNode* node = new QueueNode(event);
  if (_queue_tail == nullptr) {
    _queue_tail = _queue_head = node;
  } else {
    assert(_queue_tail->next() == nullptr, "Must be the last element in the list");
    _queue_tail->set_next(node);
    _queue_tail = node;
  }

  assert((_queue_head == nullptr) == (_queue_tail == nullptr),
         "Inconsistent queue markers");
}

JvmtiDeferredEvent JvmtiDeferredEventQueue::dequeue() {
  assert(_queue_head != nullptr, "Nothing to dequeue");

  if (_queue_head == nullptr) {
    // Just in case this happens in product; it shouldn't but let's not crash
    return JvmtiDeferredEvent();
  }

  QueueNode* node = _queue_head;
  _queue_head = _queue_head->next();
  if (_queue_head == nullptr) {
    _queue_tail = nullptr;
  }

  assert((_queue_head == nullptr) == (_queue_tail == nullptr),
         "Inconsistent queue markers");

  JvmtiDeferredEvent event = node->event();
  delete node;
  return event;
}

void JvmtiDeferredEventQueue::post(JvmtiEnv* env) {
  // Post events while nmethods are still in the queue and can't be unloaded.
  while (_queue_head != nullptr) {
    _queue_head->event().post_compiled_method_load_event(env);
    dequeue();
  }
}

void JvmtiDeferredEventQueue::run_nmethod_entry_barriers() {
  for(QueueNode* node = _queue_head; node != nullptr; node = node->next()) {
     node->event().run_nmethod_entry_barriers();
  }
}


void JvmtiDeferredEventQueue::oops_do(OopClosure* f, NMethodClosure* cf) {
  for(QueueNode* node = _queue_head; node != nullptr; node = node->next()) {
     node->event().oops_do(f, cf);
  }
}

void JvmtiDeferredEventQueue::nmethods_do(NMethodClosure* cf) {
  for(QueueNode* node = _queue_head; node != nullptr; node = node->next()) {
     node->event().nmethods_do(cf);
  }
}
