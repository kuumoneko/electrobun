import { join, resolve } from 'node:path';
import { getBinExt, isNewer } from '../../share/os';
import { build } from 'bun';
import { writeFileSync } from 'node:fs';


/**
 * 
 * @param path 
 * workspace path
 */
export default async function build_preload(path: string) {
    path = resolve(path);

    const preloadSource = join(path, "src", "bun", "preload", "index.ts");
    const outDir = join(path, "preload", ".generated");
    const outFile = join(outDir, `compiled.ts`);

    console.log("\n" + Array(10).fill("=").join("") + ` Build preload ` + Array(10).fill("=").join(""))

    if (isNewer(preloadSource, [outFile])) {
        console.log("preload is changed! Building...");

        const fullResult = await build({
            entrypoints: [preloadSource],
            minify: false,
            target: "browser",
            format: "esm",
            outdir: outDir,
        })

        const fullPreloadJs = `(function(){${await fullResult.outputs[0].text()}})();`;
        writeFileSync(outFile, `export const preloadScript = ${JSON.stringify(fullPreloadJs)};`);
        console.log(`preload build done.`)
    }
    else {
        console.log(`preload is not changed. Skipping...`)
    }
}