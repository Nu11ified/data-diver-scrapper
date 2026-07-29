// Surface of the Emscripten glue, pinned so nothing downstream is untyped.
export interface EmscriptenModule {
  ccall(
    name: string,
    returnType: "number" | null,
    argTypes: ReadonlyArray<"number">,
    args: ReadonlyArray<number>,
  ): number;
  stringToNewUTF8(text: string): number;
  UTF8ToString(pointer: number): string;
  _free(pointer: number): void;
}

export interface ModuleOptions {
  instantiateWasm?(
    imports: WebAssembly.Imports,
    onSuccess: (instance: WebAssembly.Instance, module: WebAssembly.Module) => void,
  ): Record<string, never>;
}

declare const createModule: (options?: ModuleOptions) => Promise<EmscriptenModule>;
export default createModule;
