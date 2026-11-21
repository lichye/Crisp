// The legal forms of the four constructs in rtl-*.sv.
module legal(input clk, input [3:0] a);
    typedef enum logic [2:0] { IDLE = 3'h0, RUN = 3'h1 } state_t;
    logic [3:0] q;
    state_t state = IDLE;
    always_ff @(posedge clk) begin
        q <= a;
        state <= a[0] ? RUN : IDLE;
        if (a == 4'hf) $fatal(1, "a reached 15");
    end
    p_follows: assert property (@(posedge clk) 1 |=> q == $past(a));
    p_known: assert property (@(posedge clk) state == IDLE || state == RUN);
endmodule
