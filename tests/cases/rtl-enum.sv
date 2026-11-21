// IEEE 1800-2017 6.19: a sized enum literal must have the width of the base type.
module enum_width(input clk, input go);
    typedef enum logic [2:0] { IDLE = 4'h0, RUN = 4'h1 } state_t;
    state_t state = IDLE;
    always_ff @(posedge clk) state <= go ? RUN : IDLE;
    p_known: assert property (@(posedge clk) state == IDLE || state == RUN);
endmodule
