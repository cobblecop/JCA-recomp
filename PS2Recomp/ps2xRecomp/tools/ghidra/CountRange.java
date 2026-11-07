// @category PS2Recomp
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.AddressSet;

public class CountRange extends GhidraScript {
    @Override
    public void run() throws Exception {
        AddressSet rng = new AddressSet(toAddr(0x3bc7e0), toAddr(0x3c2fff));
        int instr = 0;
        var it = currentProgram.getListing().getInstructions(rng, true);
        while (it.hasNext()) { it.next(); instr++; }
        int funcs = 0;
        var fi = currentProgram.getFunctionManager().getFunctions(rng, true);
        while (fi.hasNext()) { fi.next(); funcs++; }
        println("instructions: " + instr + ", functions: " + funcs);
    }
}