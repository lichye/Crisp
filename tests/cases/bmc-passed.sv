module bmc_passed(input wire clk);
    reg [5:0] c = 0;
    always @(posedge clk)
        c <= c + 6'd1;
    p_below_limit: assert property (@(posedge clk) c < 6'd30);
endmodule
