"""Bounded, read-only extraction of shared runtime compatibility diagnostics."""
import re
from host_paths import io_path

MAX_RECORDS = 200
MAX_LINE = 16384
MISSING_FUNCTION = re.compile(r'FATAL: no function at (?:0x)?([0-9a-fA-F]+)\s*\(lr=(?:0x)?([0-9a-fA-F]+)')
SHADER_INSTRUCTION = re.compile(r'\[shader\] required program failed: unknown instruction ([0-9a-fA-F]{16}) at (0x[0-9a-fA-F]+)(?: \(code ([0-9a-fA-F]+)\))?')
GUEST_TRAP = re.compile(r'FATAL: undefined instruction / trap at (?:0x)?([0-9a-fA-F]+)')
HOST_EXCEPTION = re.compile(r'^\*\*\* (?:exception|signal) (\S+) accessing (\S+)')


def runtime_diagnostics(path):
    """Observe failures without executing log text or inferring game readiness."""
    records, calls, discarded, lines, long_lines = {}, {}, 0, 0, 0
    with io_path(path).open('r', encoding='utf-8', errors='replace') as source:
        while True:
            line = source.readline(MAX_LINE + 1)
            if not line:
                break
            lines += 1
            if len(line) > MAX_LINE:
                long_lines += 1
                rest = line
                while rest and not rest.endswith('\n'):
                    rest = source.readline(MAX_LINE + 1)
                line = line[:MAX_LINE]
            line = line.rstrip('\r\n')
            record = None
            match = MISSING_FUNCTION.search(line)
            if match:
                record = dict(kind='missing_function', subsystem='cpu',
                              pc=hex(int(match[1], 16)), lr=hex(int(match[2], 16)))
            elif (match := SHADER_INSTRUCTION.search(line)):
                record = dict(kind='unsupported_shader_instruction', subsystem='gpu',
                              instruction='0x' + match[1].lower(), offset=hex(int(match[2], 16)))
                if match[3]: record['code_address'] = hex(int(match[3], 16))
            elif (match := GUEST_TRAP.search(line)):
                record = dict(kind='guest_instruction_trap', subsystem='cpu', pc=hex(int(match[1], 16)))
            elif (match := HOST_EXCEPTION.search(line)):
                record = dict(kind='host_exception', subsystem='runtime', exception=match[1], address=match[2])
            elif ('[hle] UNIMPLEMENTED ' in line or '[ipc] STUB ' in line or
                  'missing host implementation:' in line or 'missing service implementation:' in line):
                detail = line[:500]
                if detail not in calls and len(calls) < MAX_RECORDS: calls[detail] = None
                record = dict(kind='missing_runtime_call', subsystem='hle' if '[hle]' in line or
                              'missing host implementation:' in line else 'ipc', detail=detail)
            elif '[shader] required program failed:' in line:
                record = dict(kind='shader_translation_failure', subsystem='gpu', detail=line[:500])
            elif 'FATAL:' in line:
                record = dict(kind='fatal', subsystem='runtime', detail=line[:500])
            if record is None:
                continue
            key = tuple(sorted(record.items()))
            if key in records:
                records[key]['occurrences'] += 1
                records[key]['last_line'] = lines
            elif len(records) < MAX_RECORDS:
                records[key] = dict(record, occurrences=1, first_line=lines, last_line=lines)
            else:
                discarded += 1
    return dict(schema=1, blockers=list(records.values()), unimplemented_calls=list(calls),
                lines_scanned=lines, truncated=bool(discarded or long_lines),
                discarded_records=discarded, truncated_lines=long_lines,
                evidence='Observed log diagnostics only. An empty result does not prove compatibility, gameplay or successful exit.')
