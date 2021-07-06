#include <ruby.h>
#include <stdio.h>
#include <ddprof/ffi.h>

static VALUE native_working_p(VALUE self);
static VALUE native_send(VALUE self, VALUE url, VALUE headers, VALUE body, VALUE timeout_ms);

void Init_ddtrace_profiling_native_extension(void) {
  VALUE datadog_module = rb_define_module("Datadog");
  VALUE profiling_module = rb_define_module_under(datadog_module, "Profiling");
  VALUE native_extension_module = rb_define_module_under(profiling_module, "NativeExtension");

  rb_define_singleton_method(native_extension_module, "native_working?", native_working_p, 0);
  rb_funcall(native_extension_module, rb_intern("private_class_method"), 1, ID2SYM(rb_intern("native_working?")));

  VALUE LibddprofAdapter = rb_define_class("LibddprofAdapter", rb_cObject);
  rb_define_method(LibddprofAdapter, "native_send", native_send, 4);
}

static VALUE native_working_p(VALUE self) {
  return Qtrue;
}

static VALUE native_send(VALUE _self, VALUE url, VALUE headers, VALUE body, VALUE timeout_ms) {
  const char *c_url = StringValueCStr(url);
  ddprof_ffi_ByteSlice c_body = {.ptr = (uint8_t*) StringValuePtr(body), .len = RSTRING_LEN(body)};
  uint64_t c_timeout_ms = FIX2LONG(timeout_ms);

  // Header conversion
  long header_count = rb_array_len(headers);
  ddprof_ffi_Field fields[header_count];

  for (long i = 0; i < header_count; i++) {
    VALUE header_pair = rb_ary_entry(headers, i);
    VALUE name_string = rb_ary_entry(header_pair, 0);
    VALUE value_string = rb_ary_entry(header_pair, 1);
    Check_Type(name_string, T_STRING);
    Check_Type(value_string, T_STRING);

    fields[i] = (ddprof_ffi_Field) {
      .name = StringValueCStr(name_string),
      .value = {
        .ptr = (uint8_t*) StringValuePtr(value_string),
        .len = RSTRING_LEN(value_string)
      },
    };
  }

  ddprof_ffi_Slice_field c_headers = {
    .len = header_count,
    .ptr = fields
  };
  // Header conversion end

  ddprof_ffi_Exporter *exporter = ddprof_ffi_Exporter_new();
  ddprof_ffi_SendResult status = ddprof_ffi_Exporter_send(
      exporter, "POST", c_url, c_headers, c_body, c_timeout_ms);
  ddprof_ffi_Exporter_delete(exporter);

  if (status.tag == DDPROF_FFI_SEND_RESULT_HTTP_RESPONSE) {
    fprintf(stderr, "HTTP code: %d\n", status.http_response.code);
    return Qtrue;
  } else {
    int len = status.failure.len < INT_MAX ? (int)status.failure.len : INT_MAX;
    fprintf(stderr, "%*s\n", len, status.failure.ptr);
    ddprof_ffi_Buffer_reset(&status.failure);
    return Qfalse;
  }
}
