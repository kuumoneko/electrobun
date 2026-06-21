const { spawn } = require("child_process");
const { existsSync } = require("fs");
const { join, dirname } = require("path");

function getPlatform() {
  switch (process.platform) {
    case "win32":
      return "win";
    case "darwin":
      return "darwin";
    case "linux":
      return "linux";
    default:
      throw new Error(`Unsupported platform: ${process.platform}`);
  }
}

function getArch() {
  switch (process.arch) {
    case "arm64":
      return "arm64";
    case "x64":
      return "x64";
    default:
      throw new Error(`Unsupported architecture: ${process.arch}`);
  }
}

const platform = getPlatform();
const arch = platform === "win" ? "x64" : getArch();
const binExt = platform === "win" ? ".exe" : "";

const electrobunDir = join(__dirname, "..");
const cacheDir = join(electrobunDir, ".cache");
const cliBinary = join(cacheDir, `electrobun${binExt}`);

async function ensureCliBinary() {
  const binLocation = join(electrobunDir, "bin", "electrobun" + binExt);

  if (existsSync(binLocation)) {
    return binLocation;
  } else {
    throw new Error(
      `Not found module ${join(electrobunDir, "bin", "electrobun" + binExt)}`,
    );
  }
}

async function main() {
  try {
    const args = process.argv.slice(2);
    const cliPath = await ensureCliBinary();
    const child = spawn(cliPath, args, {
      stdio: "inherit",
      cwd: process.cwd(),
    });

    child.on("exit", (code) => {
      process.exit(code || 0);
    });

    child.on("error", (error) => {
      console.error("Failed to start electrobun CLI:", error.message);
      process.exit(1);
    });
  } catch (error) {
    console.error("Error:", error.message);
    process.exit(1);
  }
}
main();
