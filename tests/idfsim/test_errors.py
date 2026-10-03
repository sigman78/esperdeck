"""Fatal checks stop the child instead of returning to its caller."""
from pathlib import Path
import subprocess
import sys

binary = Path(sys.argv[1]).resolve()
for mode in ("success", "fail"):
    result = subprocess.run([str(binary), mode], capture_output=True,
                            text=True, timeout=10)
    assert result.stderr.count("evaluated\n") == 1, result.stderr
    if mode == "success":
        assert result.returncode == 0, result.stderr
        assert "continued" in result.stderr, result.stderr
    else:
        assert result.returncode != 0, "fatal check returned successfully"
        assert "continued" not in result.stderr, result.stderr
        for context in ("ESP_ERROR_CHECK", "checked_result(fail)", "test_errors.c", "main", "259"):
            assert context in result.stderr, result.stderr
