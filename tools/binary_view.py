"""Bounded reads and names for already-extracted binary files."""

def require(condition, message):
    if not condition:
        raise ValueError(message)


def safe_name(raw):
    name = raw.decode('utf-8')
    require(name and name not in ('.', '..') and len(raw) <= 255,
            'Invalid resource filename')
    require(not any(c in name for c in '/\\:\0<>"|?*') and
            not any(ord(c) < 32 for c in name) and name[-1] not in ' .',
            'Unsafe resource filename')
    stem = name.split('.')[0].upper()
    require(stem not in {'CON', 'PRN', 'AUX', 'NUL'} and
            not (len(stem) == 4 and stem[:3] in ('COM', 'LPT') and stem[3].isdigit()),
            'Reserved resource filename')
    return name


class View:
    def __init__(self, file, start=0, size=None):
        self.file, self.start = file, start
        if size is None:
            file.seek(0, 2)
            size = file.tell() - start
        self.size = size

    def read(self, offset, size):
        require(0 <= offset <= self.size and 0 <= size <= self.size - offset,
                'Read exceeds the binary file')
        self.file.seek(self.start + offset)
        result = self.file.read(size)
        require(len(result) == size, 'Truncated binary file')
        return result

    def sub(self, start, size):
        require(0 <= start <= self.size and 0 <= size <= self.size - start,
                'Invalid binary range')
        return View(self.file, self.start + start, size)
