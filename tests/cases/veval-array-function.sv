package veval_array_function_pkg;
    typedef logic [1:0] words_t[7:4];

    function automatic logic [1:0] mix(input words_t words, input logic [1:0] index);
        logic [1:0] acc;
        acc = words[4 + index];
        for (int step = 0; step < 9; step++)
            acc = {acc[0], acc[1]} ^ (acc + 2'b01) ^ words[7];
        return acc;
    endfunction

    function automatic words_t update_word(input words_t words, input logic [1:0] index);
        words_t updated;
        updated = words;
        updated[4 + index] = veval_array_function_pkg::mix(words, index);
        return updated;
    endfunction
endpackage

module array_function_child(
    input logic clk,
    input veval_array_function_pkg::words_t words,
    input logic [1:0] index,
    output logic [1:0] result,
    output veval_array_function_pkg::words_t copy
);
    assign result = veval_array_function_pkg::mix(words, index);
    assign copy = words;
endmodule

module array_function_top(
    input logic clk,
    input veval_array_function_pkg::words_t words,
    input logic [1:0] index
);
    logic [1:0] result;
    veval_array_function_pkg::words_t copy;
    array_function_child u_child(.*);

    p_mix: assert property (@(posedge clk)
        result == veval_array_function_pkg::mix(words, index));
`ifdef VEVAL_BAD_FUNCTION
    p_update: assert property (@(posedge clk)
        copy == veval_array_function_pkg::update_word(words, index));
`endif
endmodule
