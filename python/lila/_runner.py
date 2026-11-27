import ast
import traceback

import __main__


def run(code, filename="<script>"):
    """Exec in __main__. If the last statement is an expression, return its repr like an interactive prompt."""
    ns = __main__.__dict__
    try:
        tree = ast.parse(code, filename, "exec")
        last = None
        if tree.body and isinstance(tree.body[-1], ast.Expr):
            last = ast.Expression(tree.body.pop().value)
        exec(compile(tree, filename, "exec"), ns)
        if last is not None:
            value = eval(compile(last, filename, "eval"), ns)
            if value is not None:
                text = repr(value)
                if text:
                    print(text)
                return True, text
        return True, ""
    except SystemExit:
        raise
    except BaseException:
        traceback.print_exc()
        return False, traceback.format_exc()


def run_file(path):
    with open(path, encoding="utf-8") as f:
        code = f.read()
    ns_file = __main__.__dict__.get("__file__")
    __main__.__dict__["__file__"] = path
    try:
        return run(code, path)
    finally:
        if ns_file is None:
            __main__.__dict__.pop("__file__", None)
        else:
            __main__.__dict__["__file__"] = ns_file
