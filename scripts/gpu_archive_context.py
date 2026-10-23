"""Identity of persisted shader code and archive ABI, independent of render tuning.

Keep compiler-facing adapter sections explicit while they live in QcomMTL.mm.
Moving one requires updating the section boundary here; a missing boundary is
a build error. Ordinary render/pass implementation and bundle versions are not
shader identities. The runtime additionally incorporates the OS build string.
"""
import ast
import hashlib
import json
from pathlib import Path
import re


ABI_ROOTS = (
    'tools/gpu-compiler/SystemShaderCatalog.h',
    'drivers/gpu/QcomMTL/MetalArchive.h',
    'drivers/gpu/QcomMTL/NativeMetal.h',
    'drivers/include/GPUShaderInterface.h',
    'drivers/include/GPUShaderProtocol.h',
    'drivers/include/GPUJITProtocol.h',
)
ADAPTER = 'drivers/gpu/QcomMTL/QcomMTL.mm'
ADAPTER_SECTIONS = (
    ('registered-library', 'NSDictionary *registeredLibrary() {', 'NSError *shaderFailure('),
    ('constants', 'bool nativeFunctionConstantABI() {', 'namespace Archive=GPU::MetalArchive;'),
    ('hash', 'Archive::Key archiveHash(', 'std::string airFailureContext('),
    ('archive-digest', 'Archive::Key archiveFileDigest(', 'class ProcessAIRCache '),
    ('request-identity', 'Archive::Key archiveContext() {', 'class CompiledFunctionCache '),
    ('archive-store', 'uint64_t storeCompiledFunction(', 'NSError *archiveFailure('),
    ('pipeline-identity', 'Archive::Pipeline archivePipeline(QcomMTLComputePipeline',
     'template<class P> bool pipelineArchiveLookup('),
    ('library-load', 'bool libraryIndex(', '// Compile only unregistered input.'),
    ('library-compile', '// Compile only unregistered input.', '@implementation QcomMTLDevice\n'),
    ('function', '@implementation QcomMTLFunction\n', '@end'),
    ('program', '@implementation QcomMTLProgram\n', '@end'),
    ('archive', '@implementation QcomMTLBinaryArchive\n', '@end'),
    ('library', '@implementation QcomMTLLibrary\n', '@end'),
)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def adapter_sections(text):
    result = {}
    for name, begin, end in ADAPTER_SECTIONS:
        if text.count(begin) != 1:
            raise ValueError('Missing or ambiguous shader cache section: ' + name)
        first = text.index(begin)
        last = text.find(end, first + len(begin))
        if last < 0:
            raise ValueError('Unterminated shader cache section: ' + name)
        result[name] = digest(text[first:last].encode())
    return result


def compile_recipe(text):
    """Hash the actual frontend compile loop, without line numbers or reports.

    marshal(code) also hashes co_firstlineno and unrelated reporting code, so a
    GMEM report/build edit used to invalidate every stored function indirectly.
    """
    module = ast.parse(text)
    function = next(n for n in module.body if isinstance(n, ast.FunctionDef) and n.name == 'build_iokit_client')
    loops = [n for n in function.body if isinstance(n, ast.For) and
             isinstance(n.target, ast.Name) and n.target.id == 'source' and
             isinstance(n.iter, ast.Name) and n.iter.id == 'sources']
    if len(loops) != 1:
        raise ValueError('Cannot identify the frontend compile recipe')
    sdk = next(n for n in function.body if isinstance(n, ast.Assign) and
               any(isinstance(t, ast.Name) and t.id == 'user_sdk' for t in n.targets))
    if not (isinstance(sdk.value, ast.BinOp) and isinstance(sdk.value.op, ast.Div) and
            isinstance(sdk.value.left, ast.Name) and sdk.value.left.id == 'TOOLS' and
            isinstance(sdk.value.right, ast.Constant) and isinstance(sdk.value.right.value, str)):
        raise ValueError('Cannot identify the frontend SDK path')
    metal = next(n for n in module.body if isinstance(n, ast.FunctionDef) and n.name == 'build_gpu_metal')
    calls = [n for n in ast.walk(metal) if isinstance(n, ast.Call) and
             isinstance(n.func, ast.Name) and n.func.id == 'build_iokit_client' and n.args and
             isinstance(n.args[0], ast.Constant) and n.args[0].value == 'QcomMTLDriver']
    if len(calls) != 1:
        raise ValueError('Cannot identify the Metal frontend build')
    call = calls[0]
    sources = ['tools/' + name for name in ast.literal_eval(call.args[1]) if name.startswith('gpu-compiler/')]
    if not sources:
        raise ValueError('Metal frontend sources are absent')
    # These call-site values affect the loop's actual flags and include order.
    options = {k.arg: ast.dump(k.value, include_attributes=False) for k in call.keywords
               if k.arg in ('exception_sources', 'include_dirs', 'libraries', 'blocks')}
    return {'loop_sha256': digest(ast.dump(loops[0], include_attributes=False).encode()),
            'sdk': 'tools/' + sdk.value.right.value, 'sources': sources, 'options': options}


def build_context(root, runtime, generated, compiler_version, read=None):
    """Read inputs only; callers decide where to publish generated files."""
    root, runtime, generated = Path(root).resolve(), Path(runtime).resolve(), Path(generated).resolve()
    read = read or Path.read_bytes
    recipe = compile_recipe(read(root / 'scripts/toolchain.py').decode())
    inputs = {}
    pending = [root / name for name in (*recipe['sources'], *ABI_ROOTS)]
    include_roots = [root / 'tools/gpu-compiler', root / 'drivers/include',
                     root / 'drivers/gpu/QcomMTL']
    while pending:
        path = pending.pop()
        key = path.relative_to(root).as_posix()
        if key in inputs:
            continue
        data = read(path)
        inputs[key] = digest(data)
        for name in re.findall(r'^\s*#\s*(?:include|import)\s*[<"]([^>"]+)[>"]', data.decode(), re.M):
            for base in [path.parent, *include_roots]:
                dependency = (base / name).resolve()
                if dependency.is_file() and dependency.is_relative_to(root):
                    pending.append(dependency)
                    break
    # The actual dylib captures Mesa/backend sources, target flags and linked
    # compiler objects. Its path is not part of shader identity.
    inputs['runtime:QcomGPUCompiler.dylib'] = digest(read(runtime))
    llvm = recipe['sdk'] + '/usr/lib/libLLVM.tbd'
    inputs[llvm] = digest(read(root / llvm))
    for name in ('MTLArgument.h', 'MTLLibrary.h', 'MTLBinaryArchive.h', 'MTLFunctionConstantValues.h'):
        path = recipe['sdk'] + '/System/Library/Frameworks/Metal.framework/Headers/' + name
        inputs[path] = digest(read(root / path))
    registry = re.findall(r'constexpr uint64_t Registry = 0x([0-9a-fA-F]{16})ULL;',
                          read(generated / 'GPUPrograms.h').decode())
    if len(registry) != 1:
        raise ValueError('Missing or ambiguous AOT registry')
    policy = {
        'schema': 2,
        'compiler_version': compiler_version,
        'frontend_compile_recipe': recipe,
        'aot_registry': registry[0].lower(),
        'adapter_sections': adapter_sections(read(root / ADAPTER).decode()),
    }
    context = digest(json.dumps({'files': inputs, 'policy': policy}, sort_keys=True, separators=(',', ':')).encode())
    return {'context': context, 'inputs': inputs, 'build_policy': policy}
