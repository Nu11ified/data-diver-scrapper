declare module "*.wasm" {
  const compiled: WebAssembly.Module;
  export default compiled;
}
