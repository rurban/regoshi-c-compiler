# TCC Test Suite Report for RCC
Generated: Mon Apr 20 12:02:25 PM CEST 2026

## Summary
 - **Total**: 161
 - **Passed**: 75
 - **Failed**: 86
 - **Pass Rate**: 46%

## Detailed Results
| Test | Status | Message |
| --- | --- | --- |
| 00_assignment | PASS | Output matches |
| 01_comment | PASS | Output matches |
| 02_printf | PASS | Output matches |
| 03_struct | MISMATCH | Output does not match .expect |
| 04_for | PASS | Output matches |
| 05_array | PASS | Output matches |
| 06_case | PASS | Output matches |
| 07_function | PASS | Output matches |
| 08_while | PASS | Output matches |
| 09_do_while | PASS | Output matches |
| 100_c99array-decls | PASS | Output matches |
| 101_cleanup | EXEC_FAIL | non-zero exit |
| 102_alignas | MISMATCH | Output does not match .expect |
| 103_implicit_memmove | PASS | Output matches |
| 104_inline | COMPILE_FAIL | executable missing |
| 105_local_extern | PASS | Output matches |
| 106_versym | COMPILE_FAIL | rcc returned non-zero |
| 107_stack_safe | PASS | Output matches |
| 108_constructor | MISMATCH | Output does not match .expect |
| 109_float_struct_calling | MISMATCH | Output does not match .expect |
| 10_pointer | PASS | Output matches |
| 110_average | PASS | Output matches |
| 111_conversion | PASS | Output matches |
| 112_backtrace | COMPILE_FAIL | rcc returned non-zero |
| 113_btdll | COMPILE_FAIL | executable missing |
| 114_bound_signal | COMPILE_FAIL | rcc returned non-zero |
| 115_bound_setjmp | COMPILE_FAIL | rcc returned non-zero |
| 116_bound_setjmp2 | COMPILE_FAIL | rcc returned non-zero |
| 117_builtins | COMPILE_FAIL | executable missing |
| 118_switch | PASS | Output matches |
| 119_random_stuff | COMPILE_FAIL | rcc returned non-zero |
| 11_precedence | PASS | Output matches |
| 120_alias | COMPILE_FAIL | rcc returned non-zero |
| 121_struct_return | EXEC_FAIL | non-zero exit |
| 122_vla_reuse | COMPILE_FAIL | rcc returned non-zero |
| 123_vla_bug | COMPILE_FAIL | rcc returned non-zero |
| 124_atomic_counter | COMPILE_FAIL | rcc returned non-zero |
| 125_atomic_misc | COMPILE_FAIL | executable missing |
| 126_bound_global | MISMATCH | Output does not match .expect |
| 127_asm_goto | COMPILE_FAIL | rcc returned non-zero |
| 128_run_atexit | COMPILE_FAIL | rcc returned non-zero |
| 129_scopes | COMPILE_FAIL | rcc returned non-zero |
| 12_hashdefine | PASS | Output matches |
| 130_large_argument | COMPILE_FAIL | rcc returned non-zero |
| 131_return_struct_in_reg | COMPILE_FAIL | executable missing |
| 132_bound_test | MISMATCH | Output does not match .expect |
| 133_old_func | COMPILE_FAIL | rcc returned non-zero |
| 134_double_to_signed | COMPILE_FAIL | rcc returned non-zero |
| 135_func_arg_struct_compare | PASS | Output matches |
| 136_atomic_gcc_style | COMPILE_FAIL | rcc returned non-zero |
| 137_funcall_struct_args | EXEC_FAIL | non-zero exit |
| 13_integer_literals | PASS | Output matches |
| 14_if | PASS | Output matches |
| 15_recursion | PASS | Output matches |
| 16_nesting | PASS | Output matches |
| 17_enum | PASS | Output matches |
| 18_include | MISMATCH | Output does not match .expect |
| 19_pointer_arithmetic | PASS | Output matches |
| 20_pointer_comparison | PASS | Output matches |
| 21_char_array | PASS | Output matches |
| 22_floating_point | MISMATCH | Output does not match .expect |
| 23_type_coercion | PASS | Output matches |
| 24_math_library | MISMATCH | Output does not match .expect |
| 25_quicksort | PASS | Output matches |
| 26_character_constants | PASS | Output matches |
| 27_sizeof | PASS | Output matches |
| 28_strings | EXEC_FAIL | non-zero exit |
| 29_array_address | PASS | Output matches |
| 30_hanoi | PASS | Output matches |
| 31_args | PASS | Output matches |
| 32_led | PASS | Output matches |
| 33_ternary_op | COMPILE_FAIL | rcc returned non-zero |
| 34_array_assignment | PASS | Output matches |
| 35_sizeof | COMPILE_FAIL | rcc returned non-zero |
| 36_array_initialisers | PASS | Output matches |
| 37_sprintf | PASS | Output matches |
| 38_multiple_array_index | MISMATCH | Output does not match .expect |
| 39_typedef | COMPILE_FAIL | rcc returned non-zero |
| 40_stdio | COMPILE_FAIL | rcc returned non-zero |
| 41_hashif | MISMATCH | Output does not match .expect |
| 42_function_pointer | COMPILE_FAIL | rcc returned non-zero |
| 43_void_param | PASS | Output matches |
| 44_scoped_declarations | PASS | Output matches |
| 45_empty_for | PASS | Output matches |
| 46_grep | COMPILE_FAIL | rcc returned non-zero |
| 47_switch_return | PASS | Output matches |
| 48_nested_break | PASS | Output matches |
| 49_bracket_evaluation | PASS | Output matches |
| 50_logical_second_arg | PASS | Output matches |
| 51_static | PASS | Output matches |
| 52_unnamed_enum | PASS | Output matches |
| 54_goto | PASS | Output matches |
| 55_lshift_type | PASS | Output matches |
| 60_errors_and_warnings | COMPILE_FAIL | executable missing |
| 61_integers | COMPILE_FAIL | rcc returned non-zero |
| 64_macro_nesting | PASS | Output matches |
| 67_macro_concat | PASS | Output matches |
| 70_floating_point_literals | MISMATCH | Output does not match .expect |
| 71_macro_empty_arg | PASS | Output matches |
| 72_long_long_constant | PASS | Output matches |
| 73_arm64 | COMPILE_FAIL | rcc returned non-zero |
| 75_array_in_struct_init | COMPILE_FAIL | rcc returned non-zero |
| 76_dollars_in_identifiers | PASS | Output matches |
| 77_push_pop_macro | EXEC_FAIL | non-zero exit |
| 78_vla_label | COMPILE_FAIL | rcc returned non-zero |
| 79_vla_continue | COMPILE_FAIL | rcc returned non-zero |
| 80_flexarray | COMPILE_FAIL | rcc returned non-zero |
| 81_types | COMPILE_FAIL | rcc returned non-zero |
| 82_attribs_position | COMPILE_FAIL | rcc returned non-zero |
| 83_utf8_in_identifiers | COMPILE_FAIL | rcc returned non-zero |
| 84_hex-float | EXEC_FAIL | non-zero exit |
| 85_asm-outside-function | COMPILE_FAIL | rcc returned non-zero |
| 86_memory-model | COMPILE_FAIL | rcc returned non-zero |
| 87_dead_code | PASS | Output matches |
| 88_codeopt | PASS | Output matches |
| 89_nocode_wanted | PASS | Output matches |
| 90_struct-init | COMPILE_FAIL | rcc returned non-zero |
| 91_ptr_longlong_arith32 | PASS | Output matches |
| 92_enum_bitfield | COMPILE_FAIL | rcc returned non-zero |
| 93_integer_promotion | MISMATCH | Output does not match .expect |
| 94_generic | COMPILE_FAIL | rcc returned non-zero |
| 95_bitfields | COMPILE_FAIL | rcc returned non-zero |
| 95_bitfields_ms | COMPILE_FAIL | rcc returned non-zero |
| 96_nodata_wanted | COMPILE_FAIL | rcc returned non-zero |
| 97_utf8_string_literal | COMPILE_FAIL | rcc returned non-zero |
| 98_al_ax_extend | COMPILE_FAIL | rcc returned non-zero |
| 99_fastcall | COMPILE_FAIL | rcc returned non-zero |
| self_inc | COMPILE_FAIL | executable missing |
| test1 | PASS | Executed successfully |
| test_bitfields | PASS | Executed successfully |
| test_elif2 | COMPILE_FAIL | executable missing |
| test_elif3 | COMPILE_FAIL | executable missing |
| test_elif_simple | COMPILE_FAIL | executable missing |
| test_err | PASS | compile error as expected |
| test_func | PASS | Executed successfully |
| test_if2 | COMPILE_FAIL | executable missing |
| test_if3 | PASS | Executed successfully |
| test_if4 | COMPILE_FAIL | executable missing |
| test_if5 | COMPILE_FAIL | executable missing |
| test_if6 | COMPILE_FAIL | executable missing |
| test_if | PASS | Executed successfully |
| test_if_nested | PASS | Executed successfully |
| test_if_no_comment | EXEC_FAIL | non-zero exit |
| test_if_simple2 | EXEC_FAIL | non-zero exit |
| test_if_simple | PASS | Executed successfully |
| test_include2 | EXEC_FAIL | non-zero exit |
| test_include | EXEC_FAIL | non-zero exit |
| test_large_if | COMPILE_FAIL | executable missing |
| test_loop | PASS | Executed successfully |
| test_macro | EXEC_FAIL | non-zero exit |
| test_minimal | PASS | Executed successfully |
| test_nested_if | PASS | Executed successfully |
| test_ptr | PASS | Executed successfully |
| test_real | PASS | Executed successfully |
| test_self_include2 | EXEC_FAIL | non-zero exit |
| test_self_include | COMPILE_FAIL | rcc returned non-zero |
| test_simple2 | EXEC_FAIL | non-zero exit |
| test_simple | EXEC_FAIL | non-zero exit |
| test_str | PASS | Executed successfully |
| test_struct | PASS | Executed successfully |
| test_with_comment | PASS | Executed successfully |
