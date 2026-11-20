module top(input clk, input rst_n);
    reg q;
    always @(posedge clk) if (!rst_n) q <= 0; else q <= 1;
    p: assert property (@(posedge clk) rst_n |-> q == 0);
endmodule
