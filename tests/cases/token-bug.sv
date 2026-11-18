module token_formal(input wire clk, step, enable, sample);
    wire conflict;
    controller dut(.clk(clk), .step(step), .enable(enable), .sample(sample), .conflict(conflict));
    p_no_conflict: assert property (@(posedge clk) conflict);
endmodule
