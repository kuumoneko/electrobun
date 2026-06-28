import { join, resolve } from 'node:path';
import { getBinExt, isNewer } from '../../share/os';
import { build } from 'bun';


/**
 * 
 * @param path 
 * workspace path
 */
export default async function build_cli(path: string) {
    path = resolve(path);

    const cliSource = join(path, "src", "cli", "index.ts");
    const outDir = join(path, "cli", "build");
    const outFile = join(outDir, `electrobun${getBinExt()}`);

    console.log("\n" + Array(10).fill("=").join("") + ` Build Cli ` + Array(10).fill("=").join(""))

    if (isNewer(cliSource, [outFile])) {
        console.log("Cli is changed! Building...");

        // recheck cli source to add inputfile
        await build({
            entrypoints: [cliSource],
            target: "bun",
            format: "esm",
            outdir: outDir,
        })
        console.log(`Cli build done.`)
    }
    else {
        console.log(`Cli is not changed. Skipping...`)
    }
}