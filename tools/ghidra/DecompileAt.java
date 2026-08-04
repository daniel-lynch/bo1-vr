// Decompile the function containing each address passed as a script argument.
//
// Addresses are the game's OWN virtual addresses, exactly as they appear in
// this project's notes and in gameframe.c/camera.c -- image base 0x400000, so
// `0x6C7F80` is R_SetViewParms and needs no adjustment.
//
// WHY BY ADDRESS AND NOT BY NAME. A stripped retail PE has no symbols; every
// target we have is a number we found by disassembling, cross-referencing a
// string, or verifying against xoxor4d/t5-rtx. A name-based script (as in
// re4vr-port/ghidra_scripts) has nothing to match on here.
//
// Black Ops is built with LTCG, so a great many real function bodies are never
// auto-detected -- they are reached only by tail-jumps or computed calls. When
// no function covers the address we create one at it rather than giving up,
// which is what makes this usable on exactly the code we care about.
//
// @category BO1VR
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.decompiler.DecompiledFunction;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;

public class DecompileAt extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length == 0) {
            println("BO1DECOMP: no addresses given");
            return;
        }

        DecompInterface dec = new DecompInterface();
        if (!dec.openProgram(currentProgram)) {
            println("BO1DECOMP: could not open program for decompilation");
            return;
        }
        FunctionManager fm = currentProgram.getFunctionManager();

        for (String a : args) {
            Address addr;
            try {
                addr = currentProgram.getAddressFactory().getAddress(a);
            } catch (Exception e) {
                println("BO1DECOMP: " + a + " -- not a parseable address");
                continue;
            }
            if (addr == null) {
                println("BO1DECOMP: " + a + " -- not a parseable address");
                continue;
            }

            Function f = fm.getFunctionContaining(addr);
            boolean invented = false;
            if (f == null) {
                f = createFunction(addr, null);       // LTCG body Ghidra missed
                invented = (f != null);
            }
            if (f == null) {
                println("BO1DECOMP: " + a + " -- no function here and one could not be created");
                continue;
            }

            println("BO1DECOMP: /* ===== " + a + "  ->  " + f.getName()
                    + " @ " + f.getEntryPoint()
                    + (invented ? "  (function created by this script) " : " ")
                    + "===== */");
            try {
                DecompileResults r = dec.decompileFunction(f, 180, monitor);
                DecompiledFunction df = (r != null) ? r.getDecompiledFunction() : null;
                if (df == null) {
                    println("BO1DECOMP:   (decompile failed: "
                            + (r != null ? r.getErrorMessage() : "no results") + ")");
                } else {
                    for (String line : df.getC().split("\n")) println("BO1DECOMP: " + line);
                }
            } catch (Exception e) {
                println("BO1DECOMP:   (exception: " + e + ")");
            }
        }
        dec.dispose();
    }
}
