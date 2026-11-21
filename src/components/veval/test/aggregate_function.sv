typedef logic [7:0] reference_items_t[7:4];

module aggregate_function_dut(
    input logic clk,
    input logic [7:0] values[7:4],
    input logic [7:0] alternate[7:4],
    input logic [1:0] sel,
    output logic [7:0] result,
    output logic [7:0] mirrored[7:4]
);
    assign result = values[7];
    assign mirrored = values;
endmodule

module aggregate_function_boundary(
    input logic clk,
    input logic [7:0] values[7:4],
    input logic [7:0] alternate[7:4],
    input logic [1:0] sel
);
    logic [7:0] result;
    logic [7:0] mirrored[7:4];
    aggregate_function_dut dut(.*);

    function automatic logic [7:0] reference_mix(
        input reference_items_t items, input logic [1:0] index
    );
        logic [7:0] acc;
        acc = items[4 + index];
        for (int step = 0; step < 9; step++)
            acc = {acc[6:0], acc[7]} ^ (acc + 8'h2d) ^ items[7];
        return acc;
    endfunction

    function automatic reference_items_t reference_pick(
        input reference_items_t primary, input reference_items_t other,
        input logic [1:0] index
    );
        return reference_mix(primary, index) == 8'h11 ? other : primary;
    endfunction

    function automatic reference_items_t reference_update(
        input reference_items_t items, input logic [1:0] index
    );
        reference_items_t updated;
        updated = items;
        updated[4 + index] = reference_mix(items, index);
        return updated;
    endfunction

    reads_array: assert property (@(posedge clk)
        result == reference_mix(values, sel));
    reads_return: assert property (@(posedge clk)
        mirrored == reference_pick(values, alternate, sel));
    updates_array: assert property (@(posedge clk)
        mirrored == reference_update(values, sel));
endmodule
