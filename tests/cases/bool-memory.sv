// A memory of 1-bit elements with a dynamic write address. EBMC's SMT
// encoding stores Booleans into the array, and Yices 2.7 rejects it with
// "in store: type error".
module bool_memory(input clk, input [1:0] addr, input data);
    bit flag [0:3];
    always @(posedge clk) flag[addr] <= data;
    p_written: assert property (@(posedge clk) 1 |=> flag[$past(addr)] == $past(data));
    p_equal: assert property (@(posedge clk) flag[0] == flag[1]);
endmodule
