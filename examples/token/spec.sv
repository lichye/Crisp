module token_formal(input wire clk, input wire step, input wire enable, input wire sample);
    wire conflict;
    controller dut(.clk(clk), .step(step), .enable(enable), .sample(sample), .conflict(conflict));

    p_no_conflict: assert property (@(posedge clk) !conflict);
endmodule
