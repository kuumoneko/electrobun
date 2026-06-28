import { join, resolve } from 'node:path';
import { getBinExt, isNewer } from '../../share/os';
import { build } from 'bun';


/**
 * 
 * @param path 
 * workspace path
 */
export default async function build_mainJs(path: string) {
    path = resolve(path);

    const mainJsSource = join(path, "src", "mainjs", "main.ts");
    const outDir = join(path, "mainJs", "build");
    const outFile = join(outDir, `main.js`);

    console.log("\n" + Array(10).fill("=").join("") + ` Build mainJs ` + Array(10).fill("=").join(""))

    if (isNewer(mainJsSource, [outFile])) {
        console.log("mainJs is changed! Building...");

        await build({
            entrypoints: [mainJsSource],
            target: "bun",
            format: "esm",
            outdir: outDir,
        })
        console.log(`mainJs build done.`)
    }
    else {
        console.log(`mainJs is not changed. Skipping...`)
    }
}