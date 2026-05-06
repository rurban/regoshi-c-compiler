# TCC Test Suite Report for RCC

Generated on: 04/25/2026 07:42:56

## Summary

- **Total Tests**: 129
- **Passed**: 61
- **Failed**: 68
- **Pass Rate**: 47.29%

## Detailed Results

| Test                        | Status       | Message                       |
| --------------------------- | ------------ | ----------------------------- |
| 00_assignment               | PASS         | Output matches                |
| 01_comment                  | PASS         | Output matches                |
| 02_printf                   | PASS         | Output matches                |
| 03_struct                   | MISMATCH     | Output does not match .expect |
| 04_for                      | PASS         | Output matches                |
| 05_array                    | PASS         | Output matches                |
| 06_case                     | PASS         | Output matches                |
| 07_function                 | PASS         | Output matches                |
| 08_while                    | PASS         | Output matches                |
| 09_do_while                 | PASS         | Output matches                |
| 10_pointer                  | PASS         | Output matches                |
| 100_c99array-decls          | PASS         | Output matches                |
| 101_cleanup                 | COMPILE_FAIL | rcc exited with 1             |
| 102_alignas                 | MISMATCH     | Output does not match .expect |
| 103_implicit_memmove        | PASS         | Output matches                |
| 104_inline                  | COMPILE_FAIL | rcc exited with 1             |
| 104+\_inline                | COMPILE_FAIL | rcc exited with 1             |
| 105_local_extern            | PASS         | Output matches                |
| 106_versym                  | COMPILE_FAIL | rcc exited with 1             |
| 107_stack_safe              | PASS         | Output matches                |
| 108_constructor             | MISMATCH     | Output does not match .expect |
| 109_float_struct_calling    | COMPILE_FAIL | rcc exited with 1             |
| 11_precedence               | PASS         | Output matches                |
| 110_average                 | MISMATCH     | Output does not match .expect |
| 111_conversion              | PASS         | Output matches                |
| 112_backtrace               | MISMATCH     | Output does not match .expect |
| 113_btdll                   | COMPILE_FAIL | rcc exited with 1             |
| 114_bound_signal            | COMPILE_FAIL | rcc exited with 1             |
| 115_bound_setjmp            | COMPILE_FAIL | rcc exited with 1             |
| 116_bound_setjmp2           | COMPILE_FAIL | rcc exited with 1             |
| 117_builtins                | COMPILE_FAIL | rcc exited with 1             |
| 118_switch                  | PASS         | Output matches                |
| 119_random_stuff            | COMPILE_FAIL | rcc exited with 1             |
| 12_hashdefine               | PASS         | Output matches                |
| 120_alias                   | COMPILE_FAIL | rcc exited with 1             |
| 120+\_alias                 | COMPILE_FAIL | rcc exited with 1             |
| 121_struct_return           | COMPILE_FAIL | rcc exited with 1             |
| 122_vla_reuse               | COMPILE_FAIL | rcc exited with 1             |
| 123_vla_bug                 | COMPILE_FAIL | rcc exited with 1             |
| 124_atomic_counter          | COMPILE_FAIL | rcc exited with 1             |
| 125_atomic_misc             | COMPILE_FAIL | rcc exited with 1             |
| 126_bound_global            | MISMATCH     | Output does not match .expect |
| 127_asm_goto                | COMPILE_FAIL | rcc exited with 1             |
| 128_run_atexit              | COMPILE_FAIL | rcc exited with 1             |
| 129_scopes                  | COMPILE_FAIL | rcc exited with 1             |
| 13_integer_literals         | PASS         | Output matches                |
| 130_large_argument          | COMPILE_FAIL | rcc exited with 1             |
| 131_return_struct_in_reg    | COMPILE_FAIL | rcc exited with 1             |
| 132_bound_test              | COMPILE_FAIL | rcc exited with 1             |
| 133_old_func                | COMPILE_FAIL | rcc exited with 1             |
| 134_double_to_signed        | COMPILE_FAIL | rcc exited with -1073741819   |
| 135_func_arg_struct_compare | PASS         | Output matches                |
| 136_atomic_gcc_style        | COMPILE_FAIL | rcc exited with 1             |
| 137_funcall_struct_args     | MISMATCH     | Output does not match .expect |
| 14_if                       | PASS         | Output matches                |
| 15_recursion                | PASS         | Output matches                |
| 16_nesting                  | PASS         | Output matches                |
| 17_enum                     | PASS         | Output matches                |
| 18_include                  | PASS         | Output matches                |
| 19_pointer_arithmetic       | MISMATCH     | Output does not match .expect |
| 20_pointer_comparison       | PASS         | Output matches                |
| 21_char_array               | PASS         | Output matches                |
| 22_floating_point           | PASS         | Output matches                |
| 23_type_coercion            | MISMATCH     | Output does not match .expect |
| 24_math_library             | PASS         | Output matches                |
| 25_quicksort                | PASS         | Output matches                |
| 26_character_constants      | PASS         | Output matches                |
| 27_sizeof                   | PASS         | Output matches                |
| 28_strings                  | PASS         | Output matches                |
| 29_array_address            | PASS         | Output matches                |
| 30_hanoi                    | PASS         | Output matches                |
| 31_args                     | MISMATCH     | Output does not match .expect |
| 32_led                      | PASS         | Output matches                |
| 33_ternary_op               | COMPILE_FAIL | rcc exited with 1             |
| 34_array_assignment         | MISMATCH     | Output does not match .expect |
| 35_sizeof                   | COMPILE_FAIL | rcc exited with 1             |
| 36_array_initialisers       | MISMATCH     | Output does not match .expect |
| 37_sprintf                  | PASS         | Output matches                |
| 38_multiple_array_index     | MISMATCH     | Output does not match .expect |
| 39_typedef                  | COMPILE_FAIL | rcc exited with 1             |
| 40_stdio                    | PASS         | Output matches                |
| 41_hashif                   | PASS         | Output matches                |
| 42_function_pointer         | PASS         | Output matches                |
| 43_void_param               | PASS         | Output matches                |
| 44_scoped_declarations      | PASS         | Output matches                |
| 45_empty_for                | PASS         | Output matches                |
| 46_grep                     | MISMATCH     | Output does not match .expect |
| 47_switch_return            | PASS         | Output matches                |
| 48_nested_break             | PASS         | Output matches                |
| 49_bracket_evaluation       | PASS         | Output matches                |
| 50_logical_second_arg       | PASS         | Output matches                |
| 51_static                   | PASS         | Output matches                |
| 52_unnamed_enum             | PASS         | Output matches                |
| 54_goto                     | PASS         | Output matches                |
| 55_lshift_type              | PASS         | Output matches                |
| 60_errors_and_warnings      | COMPILE_FAIL | rcc exited with 1             |
| 61_integers                 | COMPILE_FAIL | rcc exited with -1073741819   |
| 64_macro_nesting            | PASS         | Output matches                |
| 67_macro_concat             | MISMATCH     | Output does not match .expect |
| 70_floating_point_literals  | MISMATCH     | Output does not match .expect |
| 71_macro_empty_arg          | PASS         | Output matches                |
| 72_long_long_constant       | PASS         | Output matches                |
| 73_arm64                    | COMPILE_FAIL | rcc exited with 1             |
| 75_array_in_struct_init     | COMPILE_FAIL | rcc exited with 1             |
| 76_dollars_in_identifiers   | PASS         | Output matches                |
| 77_push_pop_macro           | MISMATCH     | Output does not match .expect |
| 78_vla_label                | COMPILE_FAIL | rcc exited with 1             |
| 79_vla_continue             | COMPILE_FAIL | rcc exited with 1             |
| 80_flexarray                | COMPILE_FAIL | rcc exited with 1             |
| 81_types                    | COMPILE_FAIL | rcc exited with 1             |
| 82_attribs_position         | COMPILE_FAIL | rcc exited with 1             |
| 83_utf8_in_identifiers      | COMPILE_FAIL | rcc exited with 1             |
| 84_hex-float                | PASS         | Output matches                |
| 85_asm-outside-function     | COMPILE_FAIL | rcc exited with 1             |
| 86_memory-model             | PASS         | Output matches                |
| 87_dead_code                | PASS         | Output matches                |
| 88_codeopt                  | PASS         | Output matches                |
| 89_nocode_wanted            | PASS         | Output matches                |
| 90_struct-init              | COMPILE_FAIL | rcc exited with 1             |
| 91_ptr_longlong_arith32     | PASS         | Output matches                |
| 92_enum_bitfield            | COMPILE_FAIL | rcc exited with 1             |
| 93_integer_promotion        | COMPILE_FAIL | rcc exited with 1             |
| 94_generic                  | COMPILE_FAIL | rcc exited with 1             |
| 95_bitfields_ms             | COMPILE_FAIL | rcc exited with 1             |
| 95_bitfields                | MISMATCH     | Output does not match .expect |
| 96_nodata_wanted            | COMPILE_FAIL | rcc exited with 1             |
| 97_utf8_string_literal      | COMPILE_FAIL | rcc exited with -1073741819   |
| 98_al_ax_extend             | COMPILE_FAIL | rcc exited with 1             |
| 99_fastcall                 | COMPILE_FAIL | rcc exited with 1             |
