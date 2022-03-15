#include <ruby.h>
#include <ruby/debug.h>
#include <ddprof/ffi.h>

static unsigned long allocation_count = 0;
static VALUE current_collector = Qnil;
static VALUE allocation_tracepoint = Qnil;
static VALUE missing_string = Qnil;
static VALUE wip_memory_module = Qnil;

// collectors_stack.c
VALUE create_stack_collector();
void collector_add(VALUE collector, ddprof_ffi_Sample sample);

// Hack  -- this is not on the public Ruby headers
extern size_t rb_obj_memsize_of(VALUE);
extern int rb_objspace_garbage_object_p(VALUE obj);
extern const struct st_hash_type rb_hashtype_ident;

static void on_newobj_event(VALUE tracepoint_info, void *_unused);
static void record_sample(int stack_depth, VALUE *stack_buffer, int *lines_buffer, int size_bytes);
static VALUE get_allocation_count(VALUE self);
static VALUE get_current_collector(VALUE self);
static VALUE start_allocation_tracing(VALUE self);
static VALUE stop_allocation_tracing(VALUE self);
static VALUE flush_heap_to_collector(VALUE self);
static void track_object(VALUE newobject);

static st_table *tracked_objects = NULL;

void wip_memory_init(VALUE profiling_module) {
  wip_memory_module = rb_define_module_under(profiling_module, "WipMemory");

  // Experimental support for allocation tracking
  rb_define_singleton_method(wip_memory_module, "start_allocation_tracing", start_allocation_tracing, 0);
  rb_define_singleton_method(wip_memory_module, "stop_allocation_tracing", stop_allocation_tracing, 0);
  rb_define_singleton_method(wip_memory_module, "allocation_count", get_allocation_count, 0);
  rb_define_singleton_method(wip_memory_module, "current_collector", get_current_collector, 0);
  rb_define_singleton_method(wip_memory_module, "flush_heap_to_collector", flush_heap_to_collector, 0);

  current_collector = create_stack_collector();
  tracked_objects = rb_st_init_numtable(); // Hashmap with "numbers" as keys

  allocation_tracepoint = rb_tracepoint_new(
    0, // all threads
    RUBY_INTERNAL_EVENT_NEWOBJ, // object allocation,
    on_newobj_event,
    0 // unused
  );

  rb_global_variable(&current_collector);
  rb_global_variable(&allocation_tracepoint);

  missing_string = rb_str_new2("(nil)");
  rb_global_variable(&missing_string);
}

static void on_newobj_event(VALUE tracepoint_info, void *_unused) {
  allocation_count++;

  rb_trace_arg_t *tparg = rb_tracearg_from_tracepoint(tracepoint_info);

  int buffer_max_size = 1024;
  VALUE stack_buffer[buffer_max_size];
  int lines_buffer[buffer_max_size];

  int stack_depth = rb_profile_frames(
    0, // stack starting depth
    buffer_max_size,
    stack_buffer,
    lines_buffer
  );

  VALUE allocated_object = rb_tracearg_object(tparg);
  record_sample(stack_depth, stack_buffer, lines_buffer, rb_obj_memsize_of(allocated_object));

  track_object(allocated_object);
}

static void record_sample(int stack_depth, VALUE *stack_buffer, int *lines_buffer, int size_bytes) {
  ddprof_ffi_Location locations[stack_depth];
  ddprof_ffi_Line lines[stack_depth];

  for (int i = 0; i < stack_depth; i++) {
    VALUE name = rb_profile_frame_full_label(stack_buffer[i]);
    VALUE filename = rb_profile_frame_absolute_path(stack_buffer[i]);
    if (NIL_P(filename)) {
      filename = rb_profile_frame_path(stack_buffer[i]);
    }

    name = NIL_P(name) ? missing_string : name;
    filename = NIL_P(filename) ? missing_string : filename;

    locations[i] = (ddprof_ffi_Location){.lines = (ddprof_ffi_Slice_line){&lines[i], 1}};
    lines[i] = (ddprof_ffi_Line){
      .function = (ddprof_ffi_Function){
        .name = {StringValuePtr(name), RSTRING_LEN(name)},
        .filename = {StringValuePtr(filename), RSTRING_LEN(filename)}
      },
      .line = lines_buffer[i],
    };
  }

  int64_t count = 1; // single object allocated
  int64_t metrics[] = {count, size_bytes};

  ddprof_ffi_Sample sample = {
    .locations = {locations, stack_depth},
    .values = {metrics, 2}
  };

  collector_add(current_collector, sample);
}

static VALUE get_allocation_count(VALUE self) {
  return ULONG2NUM(allocation_count);
}

static VALUE get_current_collector(VALUE self) {
  return current_collector;
}

static VALUE start_allocation_tracing(VALUE self) {
  rb_tracepoint_enable(allocation_tracepoint);

  return allocation_tracepoint;
}

static VALUE stop_allocation_tracing(VALUE self) {
  rb_tracepoint_disable(allocation_tracepoint);

  // if (!ddprof_ffi_Profile_reset(allocation_profile)) rb_raise(rb_eRuntimeError, "Failed to reset profile");

  return Qtrue;
}

static void track_object(VALUE newobject) {
  VALUE object_id = rb_obj_id(newobject);

  // TODO FIXME
  ddprof_ffi_Location *stack_trace = NULL;

  // TODO: Is NUM2ULONG safe for all possible object_id values? Test what happens for really large values (and negative ones)
  rb_st_insert(tracked_objects, NUM2ULONG(object_id), (st_data_t) stack_trace);
}

static VALUE maybe_get_size(VALUE object_id) {
  return rb_funcall(wip_memory_module, rb_intern_const("maybe_get_size"), 1, object_id);
}

static int flush_object_to_collector(st_data_t key, st_data_t value, st_data_t data) {
  VALUE object_id = ULONG2NUM(key);

  VALUE object_size_if_alive = maybe_get_size(object_id);

  if (!RTEST(object_size_if_alive)) {
    printf("Object with id %u is no longer alive\n", key);

    return ST_DELETE; // Object is no longer alive
  }

  // add sample to collector
  printf("Will add object with id %u and size %u to collector\n", key, NUM2ULONG(object_size_if_alive));

  return ST_CONTINUE;
}

static VALUE flush_heap_to_collector(VALUE self) {
  rb_st_foreach(tracked_objects, flush_object_to_collector, NULL);

  return Qtrue;
}
