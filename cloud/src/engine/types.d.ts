// The .wasm artifact is bundled by Alchemy as a CompiledWasm module.
declare module "*.wasm" {
  const compiled: WebAssembly.Module;
  export default compiled;
}
