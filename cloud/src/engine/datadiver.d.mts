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

declare const createModule: () => Promise<EmscriptenModule>;
export default createModule;
