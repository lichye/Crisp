module top(input clk, input [0:0] rst_n, input [1:0] wide, output wire outgoing, inout wire bidir);
    reg busy = 0;
    reg [1:0] c = 0;
    wire internal = c[0];
    assign outgoing = internal;
    always @(posedge clk) begin busy <= 0; c <= c + 1; end
    p: assert property (@(posedge clk) c == 0);
endmodule
