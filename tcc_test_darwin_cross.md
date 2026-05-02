# TCC Test Suite Report for RCC
Generated: May 2026

## Summary
 - **Total**:     141
 - **Passed**:    134
 - **Failed**:    7
 - **Pass Rate**: 95%

## Detailed Results
| Test                                     | Status       | Message                              |
|:-----------------------------------------|:-------------|:-------------------------------------|
| 00_assignment                            | COMPILE_OK   | linked, (execution skipped)          |
| 01_comment                               | COMPILE_OK   | linked, (execution skipped)          |
| 02_printf                                | COMPILE_OK   | linked, (execution skipped)          |
| 03_struct                                | COMPILE_OK   | linked, (execution skipped)          |
| 04_for                                   | COMPILE_OK   | linked, (execution skipped)          |
| 05_array                                 | COMPILE_OK   | linked, (execution skipped)          |
| 06_case                                  | COMPILE_OK   | linked, (execution skipped)          |
| 07_function                              | COMPILE_OK   | linked, (execution skipped)          |
| 08_while                                 | COMPILE_OK   | linked, (execution skipped)          |
| 09_do_while                              | COMPILE_OK   | linked, (execution skipped)          |
| 10_pointer                               | COMPILE_OK   | linked, (execution skipped)          |
| 11_precedence                            | COMPILE_OK   | linked, (execution skipped)          |
| 12_hashdefine                            | COMPILE_OK   | linked, (execution skipped)          |
| 13_integer_literals                      | COMPILE_OK   | linked, (execution skipped)          |
| 14_if                                    | COMPILE_OK   | linked, (execution skipped)          |
| 15_recursion                             | COMPILE_OK   | linked, (execution skipped)          |
| 16_nesting                               | COMPILE_OK   | linked, (execution skipped)          |
| 17_enum                                  | COMPILE_OK   | linked, (execution skipped)          |
| 18_include                               | COMPILE_OK   | linked, (execution skipped)          |
| 19_pointer_arithmetic                    | COMPILE_OK   | linked, (execution skipped)          |
| 20_pointer_comparison                    | COMPILE_OK   | linked, (execution skipped)          |
| 21_char_array                            | COMPILE_OK   | linked, (execution skipped)          |
| 22_floating_point                        | COMPILE_OK   | linked, (execution skipped)          |
| 23_type_coercion                         | COMPILE_OK   | linked, (execution skipped)          |
| 24_math_library                          | COMPILE_OK   | linked, (execution skipped)          |
| 25_quicksort                             | COMPILE_OK   | linked, (execution skipped)          |
| 26_character_constants                   | COMPILE_OK   | linked, (execution skipped)          |
| 27_sizeof                                | COMPILE_OK   | linked, (execution skipped)          |
| 28_strings                               | COMPILE_OK   | linked, (execution skipped)          |
| 29_array_address                         | COMPILE_OK   | linked, (execution skipped)          |
| 30_hanoi                                 | COMPILE_OK   | linked, (execution skipped)          |
| 31_args                                  | COMPILE_OK   | linked, (execution skipped)          |
| 32_led                                   | COMPILE_OK   | linked, (execution skipped)          |
| 33_ternary_op                            | COMPILE_OK   | linked, (execution skipped)          |
| 34_array_assignment                      | COMPILE_OK   | linked, (execution skipped)          |
| 35_sizeof                                | COMPILE_OK   | linked, (execution skipped)          |
| 36_array_initialisers                    | COMPILE_OK   | linked, (execution skipped)          |
| 37_sprintf                               | COMPILE_OK   | linked, (execution skipped)          |
| 38_multiple_array_index                  | COMPILE_OK   | linked, (execution skipped)          |
| 39_typedef                               | COMPILE_OK   | linked, (execution skipped)          |
| 40_stdio                                 | COMPILE_OK   | linked, (execution skipped)          |
| 41_hashif                                | COMPILE_OK   | linked, (execution skipped)          |
| 42_function_pointer                      | COMPILE_FAIL | rcc returned non-zero                |
| 43_void_param                            | COMPILE_OK   | linked, (execution skipped)          |
| 44_scoped_declarations                   | COMPILE_OK   | linked, (execution skipped)          |
| 45_empty_for                             | COMPILE_OK   | linked, (execution skipped)          |
| 46_grep                                  | COMPILE_FAIL | rcc returned non-zero                |
| 47_switch_return                         | COMPILE_OK   | linked, (execution skipped)          |
| 48_nested_break                          | COMPILE_OK   | linked, (execution skipped)          |
| 49_bracket_evaluation                    | COMPILE_OK   | linked, (execution skipped)          |
| 50_logical_second_arg                    | COMPILE_OK   | linked, (execution skipped)          |
| 51_static                                | COMPILE_OK   | linked, (execution skipped)          |
| 52_unnamed_enum                          | COMPILE_OK   | linked, (execution skipped)          |
| 54_goto                                  | COMPILE_OK   | linked, (execution skipped)          |
| 55_lshift_type                           | COMPILE_OK   | linked, (execution skipped)          |
| 60_errors_and_warnings                   | SKIP         | Skipped                              |
| 61_integers                              | COMPILE_OK   | linked, (execution skipped)          |
| 64_macro_nesting                         | COMPILE_OK   | linked, (execution skipped)          |
| 67_macro_concat                          | COMPILE_OK   | linked, (execution skipped)          |
| 70_floating_point_literals               | COMPILE_OK   | linked, (execution skipped)          |
| 71_macro_empty_arg                       | COMPILE_OK   | linked, (execution skipped)          |
| 72_long_long_constant                    | COMPILE_OK   | linked, (execution skipped)          |
| 73_arm64                                 | SKIP         | Skipped                              |
| 75_array_in_struct_init                  | COMPILE_OK   | linked, (execution skipped)          |
| 76_dollars_in_identifiers                | COMPILE_OK   | linked, (execution skipped)          |
| 77_push_pop_macro                        | COMPILE_OK   | linked, (execution skipped)          |
| 78_vla_label                             | COMPILE_OK   | linked, (execution skipped)          |
| 79_vla_continue                          | COMPILE_OK   | linked, (execution skipped)          |
| 80_flexarray                             | COMPILE_OK   | linked, (execution skipped)          |
| 81_types                                 | COMPILE_OK   | linked, (execution skipped)          |
| 82_attribs_position                      | COMPILE_OK   | linked, (execution skipped)          |
| 83_utf8_in_identifiers                   | COMPILE_OK   | linked, (execution skipped)          |
| 84_hex-float                             | COMPILE_OK   | linked, (execution skipped)          |
| 85_asm-outside-function                  | COMPILE_FAIL | rcc returned non-zero                |
| 86_memory-model                          | COMPILE_OK   | linked, (execution skipped)          |
| 87_dead_code                             | COMPILE_OK   | linked, (execution skipped)          |
| 88_codeopt                               | COMPILE_OK   | linked, (execution skipped)          |
| 89_nocode_wanted                         | COMPILE_OK   | linked, (execution skipped)          |
| 90_struct-init                           | COMPILE_OK   | linked, (execution skipped)          |
| 91_ptr_longlong_arith32                  | COMPILE_OK   | linked, (execution skipped)          |
| 92_enum_bitfield                         | COMPILE_OK   | linked, (execution skipped)          |
| 93_integer_promotion                     | COMPILE_OK   | linked, (execution skipped)          |
| 94_generic                               | COMPILE_OK   | linked, (execution skipped)          |
| 95_bitfields                             | COMPILE_OK   | linked, (execution skipped)          |
| 95_bitfields_ms                          | COMPILE_OK   | linked, (execution skipped)          |
| 96_nodata_wanted                         | SKIP         | Skipped                              |
| 97_utf8_string_literal                   | COMPILE_OK   | linked, (execution skipped)          |
| 98_al_ax_extend                          | SKIP         | Skipped                              |
| 99_fastcall                              | SKIP         | Skipped                              |
| 100_c99array-decls                       | COMPILE_OK   | linked, (execution skipped)          |
| 101_cleanup                              | COMPILE_OK   | linked, (execution skipped)          |
| 102_alignas                              | COMPILE_OK   | linked, (execution skipped)          |
| 103_implicit_memmove                     | COMPILE_OK   | linked, (execution skipped)          |
| 104_inline                               | COMPILE_FAIL | rcc returned non-zero                |
| 105_local_extern                         | COMPILE_OK   | linked, (execution skipped)          |
| 106_versym                               | COMPILE_OK   | linked, (execution skipped)          |
| 107_stack_safe                           | COMPILE_OK   | linked, (execution skipped)          |
| 108_constructor                          | COMPILE_OK   | linked, (execution skipped)          |
| 109_float_struct_calling                 | COMPILE_OK   | linked, (execution skipped)          |
| 110_average                              | COMPILE_OK   | linked, (execution skipped)          |
| 111_conversion                           | COMPILE_OK   | linked, (execution skipped)          |
| 112_backtrace                            | SKIP         | Skipped                              |
| 113_btdll                                | SKIP         | Skipped                              |
| 114_bound_signal                         | SKIP         | Skipped                              |
| 115_bound_setjmp                         | SKIP         | Skipped                              |
| 116_bound_setjmp2                        | SKIP         | Skipped                              |
| 117_builtins                             | COMPILE_FAIL | rcc returned non-zero                |
| 118_switch                               | COMPILE_OK   | linked, (execution skipped)          |
| 119_random_stuff                         | COMPILE_OK   | linked, (execution skipped)          |
| 120_alias                                | SKIP         | Skipped                              |
| 121_struct_return                        | COMPILE_OK   | linked, (execution skipped)          |
| 122_vla_reuse                            | COMPILE_OK   | linked, (execution skipped)          |
| 123_vla_bug                              | COMPILE_OK   | linked, (execution skipped)          |
| 124_atomic_counter                       | SKIP         | Skipped                              |
| 125_atomic_misc                          | SKIP         | Skipped                              |
| 126_bound_global                         | SKIP         | Skipped                              |
| 127_asm_goto                             | COMPILE_FAIL | rcc returned non-zero                |
| 128_run_atexit                           | COMPILE_OK   | linked, (execution skipped)          |
| 129_scopes                               | COMPILE_FAIL | rcc returned non-zero                |
| 130_large_argument                       | COMPILE_OK   | linked, (execution skipped)          |
| 131_return_struct_in_reg                 | COMPILE_OK   | linked, (execution skipped)          |
| 132_bound_test                           | COMPILE_OK   | linked, (execution skipped)          |
| 133_old_func                             | COMPILE_OK   | linked, (execution skipped)          |
| 134_double_to_signed                     | COMPILE_OK   | linked, (execution skipped)          |
| 135_func_arg_struct_compare              | COMPILE_OK   | linked, (execution skipped)          |
| 136_atomic_gcc_style                     | SKIP         | Skipped                              |
| 137_funcall_struct_args                  | COMPILE_OK   | linked, (execution skipped)          |
| test_bitfields                           | PASS         | exit=126                             |
| test_builtins                            | PASS         | exit=126                             |
| test_elif2                               | PASS         | exit=126                             |
| test_elif_simple                         | PASS         | exit=126                             |
| test_err                                 | PASS         | compile error as expected            |
| test_func                                | PASS         | exit=126                             |
| test_if                                  | PASS         | exit=126                             |
| test_if2                                 | PASS         | exit=126                             |
| test_if3                                 | PASS         | exit=126                             |
| test_if4                                 | PASS         | exit=126                             |
| test_if5                                 | PASS         | exit=126                             |
| test_if6                                 | PASS         | exit=126                             |
| test_if_nested                           | PASS         | exit=126                             |
| test_if_simple                           | PASS         | exit=126                             |
| test_include                             | PASS         | exit=126                             |
| test_include2                            | PASS         | exit=126                             |
| test_loop                                | PASS         | exit=126                             |
| test_macro                               | PASS         | exit=126                             |
| test_minimal                             | PASS         | exit=126                             |
| test_nested_if                           | PASS         | exit=126                             |
| test_ptr                                 | PASS         | exit=126                             |
| test_real                                | PASS         | exit=126                             |
| test_self_include2                       | PASS         | exit=126                             |
| test_signextend                          | PASS         | exit=126                             |
| test_simple                              | PASS         | exit=126                             |
| test_simple2                             | PASS         | exit=126                             |
| test_str                                 | PASS         | exit=126                             |
| test_struct                              | PASS         | exit=126                             |
| test_with_comment                        | PASS         | exit=126                             |
