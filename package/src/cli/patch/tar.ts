export default async function createTar(tarPath: string, cwd: string, distPaths: string[]) {
    const tarCommand = Bun.which("tar");
    if (tarCommand === null) throw new Error("tar is not ")

    await Bun.spawn([tarCommand, "-cf", `${tarPath}`, ...distPaths], {
        cwd: cwd,
        stdout: "inherit",
        stderr: "inherit",
        stdin: "ignore",
    }).exited
}