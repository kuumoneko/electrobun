import { resolve } from "node:path"
import { getArch, getOS } from "../../share/os";
import { config } from "dotenv";
import { parseArgs } from "node:util";
import { runZigbuild } from "../../share/zig";

config();

/**
 * 
 * @param path 
 * workspace path
 */
export default async function build_launcher(path: string) {
    path = resolve(path);
    let zigArgs: string[] = [];
    const OS = getOS();
    const ARCH = getArch();
    if (OS === "win") {
        zigArgs = ["-Dtarget=x86_64-windows", "-Dcpu=baseline"];
    } else if (OS === "linux") {
        zigArgs = ARCH === "arm64" ? ["-Dtarget=aarch64-linux"] : ["-Dtarget=x86_64-linux"];
    } else if (OS === "macos") {
        zigArgs = ARCH === "arm64" ? ["-Dtarget=aarch64-macos"] : ["-Dtarget=x86_64-macos"];
    }

    const isDebug = parseArgs({
        args: Bun.argv,
        options: {
            debug: { type: "boolean" }
        }
    }).values.debug;

    if (!isDebug) {
        zigArgs.push("-Doptimize=ReleaseSmall");
    }
    await runZigbuild(path, "launcher", zigArgs);
}