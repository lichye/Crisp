module counter(input wire clk, en);
    reg [3:0] c = 0;
    always @(posedge clk)
        if (en && c < `STOP) c <= c + 1'b1;
    p_range: assert property (@(posedge clk) c <= 9);
    p_stop: assert property (@(posedge clk) c <= `STOP);
endmodule
