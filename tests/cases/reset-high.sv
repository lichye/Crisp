module top(input clk, input rst);
    reg q;
    reg [1:0] c = 0;
    always @(posedge clk) if (c != 3) c <= c + 1;
    always @(posedge clk) if (rst) q <= 0; else q <= 1;
    p: assert property (@(posedge clk) c >= 2 |-> q == 1);
endmodule
