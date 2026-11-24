// ExportFunctions.java - Ghidra headless script for recomp/x86rec.py.
//
// Writes the function list the recompiler needs as JSON:
//   [{"a":"0x401000","name":"F_00401000","thunk":false,"insns":[4198400,...]},
//    {"a":"0x4afa10","name":"thunk_...","thunk":true,"target":4198400}, ...]
//
// Usage (Ghidra 11.x, one time; the result is derived from your own HDKWIN.EXE and stays local):
//   analyzeHeadless <proj_dir> hk -import original-data/HDKWIN.EXE \
//       -scriptPath re/ghidra -postScript ExportFunctions.java build/hk-recomp/re/functions.json
// Then:
//   HK_EXE=original-data/HDKWIN.EXE HK_FUNCS=build/hk-recomp/re/functions.json \
//   HK_GEN=build/hk-recomp/gen python -I recomp/x86rec.py
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import java.io.File;
import java.io.PrintWriter;

public class ExportFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if (args.length < 1) {
            printerr("usage: ExportFunctions.java <out.json>");
            return;
        }
        File out = new File(args[0]);
        File dir = out.getParentFile();
        if (dir != null) dir.mkdirs();

        StringBuilder sb = new StringBuilder("[");
        boolean first = true;
        FunctionManager fm = currentProgram.getFunctionManager();
        int nfunc = 0, nthunk = 0;
        for (Function f : fm.getFunctions(true)) {
            Address ep = f.getEntryPoint();
            if (!f.getBody().getAddresses(true).hasNext()) continue;   // empty body: skip
            sb.append(first ? "" : ",");
            first = false;
            sb.append("{\"a\":\"").append(ep).append("\",\"name\":\"").append(f.getName()).append("\",\"thunk\":");
            boolean thunk = f.isThunk();
            sb.append(thunk);
            if (thunk) {
                Function t = f.getThunkedFunction(true);
                long target = t != null ? t.getEntryPoint().getOffset() : 0;
                sb.append(",\"target\":").append(target);
                nthunk++;
            } else {
                sb.append(",\"insns\":[");
                boolean f2 = true;
                InstructionIterator it = currentProgram.getListing().getInstructions(f.getBody(), true);
                while (it.hasNext()) {
                    Instruction i = it.next();
                    sb.append(f2 ? "" : ",").append(i.getAddress().getOffset());
                    f2 = false;
                }
                sb.append("]");
                nfunc++;
            }
            sb.append("}");
        }
        sb.append("]");
        try (PrintWriter w = new PrintWriter(out, "UTF-8")) {
            w.print(sb);
        }
        println("ExportFunctions: " + nfunc + " functions, " + nthunk + " thunks -> " + out.getAbsolutePath());
    }
}