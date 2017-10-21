open Common;

/* For tokens that are opening or closing the difference can be abstracted w/o their value */
type pairwiseKind =
  | Opening
  | Closing;

/* For Quote tokens and the string loop to identify the closing quote */
type quoteKind =
  | Double
  | Single;

/* A token's value represented by its type constructor and the value type */
type tokenValue =
  | Interpolation interpolation
  | Paren pairwiseKind
  | Brace pairwiseKind
  | Bracket pairwiseKind
  | Word string
  | AtWord string
  | Str string
  | Quote quoteKind
  | WordCombinator
  | Exclamation
  | Equal
  | Colon
  | Semicolon
  | Plus
  | Ampersand
  | Arrow
  | Asterisk
  | Tilde
  | Comma
  | Pipe
  | Dollar
  | EOF;

/* A token represented by its value and a line number */
type token = Token tokenValue int;

/* Stream type for the LexerStream */
type lexerStream = LazyStream.t token;

/* indicates whether the unquoted argument is allowed to contain more
   parentheses and whitespaces or not */
type argumentMode =
  | StrictString
  | LooseNested;

/* Modes the lexer can be in, allowing encapsulated and specialised logic */
type lexerMode =
  | MainLoop
  | CombinatorLoop
  | StringEndLoop
  | StringLoop quoteKind
  | UnquotedArgumentLoop argumentMode;

/* Running state for tokenisation */
type state = {
  /* value to keep track of the current line number; must be updated for every newline */
  mutable line: int,
  /* a buffer holding the last emitted tokenValue */
  mutable lastTokenValue: tokenValue,
  /* the current mode of the lexer */
  mutable mode: lexerMode
};

let lexer (s: Input.inputStream) => {
  let state = {
    line: 1,
    lastTokenValue: EOF,
    mode: MainLoop
  };

  /* checks whether next input value is equal to char */
  let isNextCharEqual (matching: char) => {
    switch (LazyStream.peek s) {
      | Some (Char c) when (c === matching) => true
      | _ => false
    }
  };

  /* recognise all chars that are valid starts of a word */
  let isWordStartChar (c: char) => {
    switch c {
      | 'a'..'z'
      | 'A'..'Z'
      | '0'..'9'
      | '#'
      | '.' => true
      | _ => false
    }
  };

  /* recognise all chars that are valid hex digits */
  let isHexDigitChar (c: char) => {
    switch c {
      | 'a'..'f'
      | 'A'..'F'
      | '0'..'9' => true
      | _ => false
    }
  };

  /* skip over all chars until end of comment is reached */
  let rec skipCommentContent () => {
    switch (LazyStream.next s) {
      /* end a comment on asterisk + slash */
      | Some (Char '*') when (isNextCharEqual '/') => {
        LazyStream.junk s; /* throw away the trailing slash */
      }

      /* count up current line number inside comments */
      | Some (Char '\n') => {
        state.line = state.line + 1;
        skipCommentContent ()
      }

      /* any char is recursively ignored */
      | Some _ => skipCommentContent ()

      /* a comment should be closed, since we don't want to deal with input weirdness */
      | None => {
        raise (LazyStream.Error "Unexpected EOF, expected end of comment")
      }
    };
  };

  /* skip all whitespace-like characters */
  let rec skipWhitespaces () => {
    switch (LazyStream.peek s) {
      /* count up current line number */
      | Some (Char '\n') => {
        LazyStream.junk s; /* skip over newline */
        state.line = state.line + 1;
        skipWhitespaces ()
      }

      | Some (Char ' ')
      | Some (Char '\t')
      | Some (Char '\r') => {
        LazyStream.junk s; /* skip over whitespace-like char */
        skipWhitespaces ()
      }

      /* end sequence on all other characters */
      | _ => ()
    }
  };

  /* captures any hex code of 1-n digits (spec-compliancy is 1-6), assuming that the code's start is in `str` */
  let rec captureHexDigits (str: string): string => {
    switch (LazyStream.peek s) {
      /* recursively capture all hex code characters */
      | Some (Char c) when (isHexDigitChar c) => {
        LazyStream.junk s;
        captureHexDigits (str ^ (string_of_char c))
      }

      /* hex code escapes are optionally followed by an extra whitespace */
      | Some (Char ' ') => {
        LazyStream.junk s;
        str ^ " "
      }

      /* non-matched characters end the hex sequence */
      | Some _
      | None => str
    }
  };

  /* captures the content of an escape, assuming a backslash preceded it */
  let captureEscapedContent (): string => {
    let escaped = switch (LazyStream.next s) {
      /* detect start of a hex code as those have a different escape syntax */
      | Some (Char c) when (isHexDigitChar c) => {
        captureHexDigits (string_of_char c)
      }

      /* count up current line number even for escaped newlines */
      | Some (Char '\n') => {
        state.line = state.line + 1;
        "\n"
      }

      /* capture a single non-hex char as the escaped content (spec-compliancy disallows newlines, but they are supported in practice) */
      | Some (Char c) => string_of_char c

      /* it's too risky to allow value-interpolations as part of escapes */
      | Some (Interpolation _) => {
        raise (LazyStream.Error "Unexpected interpolation after backslash, expected escaped content")
      }

      /* an escape (backslash) must be followed by at least another char, thus an EOF is unacceptable */
      | None => {
        raise (LazyStream.Error "Unexpected EOF after backslash, expected escaped content")
      }
    };

    "\\" ^ escaped
  };

  /* captures the content of a word, assuming that the word's start is captured in `str` */
  let rec captureWordContent (str: string): string => {
    switch (LazyStream.peek s) {
      /* escaped content is allowed anywhere inside a word */
      | Some (Char '\\') => {
        LazyStream.junk s;
        captureWordContent (str ^ (captureEscapedContent ()))
      }

      /* the following ranges of chars are part of the word's tail */
      | Some (Char ('%' as c))
      | Some (Char ('-' as c))
      | Some (Char ('_' as c)) => {
        LazyStream.junk s;
        captureWordContent (str ^ (string_of_char c))
      }

      | Some (Char c) when (isWordStartChar c) => {
        LazyStream.junk s;
        captureWordContent (str ^ (string_of_char c))
      }

      /* all non-word characters end the sequence */
      | Some _
      | None => {
        str
      }
    }
  };

  /* tokenise unquoted arguments like the content of url() or calc() */
  let rec unquotedArgumentLoop (kind: argumentMode) (nestedness: int) (str: string): tokenValue => {
    switch (LazyStream.peek s, kind, nestedness) {
      /* escaped content is allowed anywhere inside an unquoted argument */
      | (Some (Char '\\'), _, _) => {
        LazyStream.junk s; /* throw away leading backslash */
        unquotedArgumentLoop kind nestedness (str ^ (captureEscapedContent ()))
      }

      /* exit UnquotedArgumentLoop when closing parenthesis is found and nestedness is 0 */
      | (Some (Char ')'), _, 0) => {
        state.mode = MainLoop;
        Str str
      }

      /* count nestedness down when closing parenthesis is found */
      | (Some (Char ')'), LooseNested, _) => {
        LazyStream.junk s; /* throw away closing parenthesis */
        unquotedArgumentLoop kind (nestedness - 1) (str ^ ")")
      }

      /* nested arguments (parentheses) inside StrictString arguments are not allowed */
      | (Some (Char '('), StrictString, _) => {
        raise (LazyStream.Error "Unexpected opening parenthesis inside an unquoted argument")
      }

      | (Some (Char '('), LooseNested, _) => {
        LazyStream.junk s; /* throw away opening parenthesis */
        unquotedArgumentLoop kind (nestedness + 1) (str ^ "(")
      }

      /* in url() whitespaces can only appear at the end of a StrictString unquoted argument */
      | (Some (Char ' '), StrictString, _)
      | (Some (Char '\t'), StrictString, _)
      | (Some (Char '\r'), StrictString, _)
      | (Some (Char '\n'), StrictString, _) => {
        skipWhitespaces ();

        switch (LazyStream.peek s) {
          | Some (Char ')') => {
            state.mode = MainLoop;
            Str str
          }
          | _ => raise (LazyStream.Error "Unexpected whitespace, expected closing parenthesis")
        }
      }

      /* in calc() whitespaces can appear anywhere, so we have to count newlines */
      | (Some (Char '\n'), LooseNested, _) => {
        state.line = state.line + 1;
        unquotedArgumentLoop kind nestedness (str ^ "\n")
      }

      /* every char is accepted into the unquoted argument */
      | (Some (Char c), _, _) => {
        LazyStream.junk s; /* throw away char */
        unquotedArgumentLoop kind nestedness (str ^ (string_of_char c))
      }

      /* emit string and wait for next loop when encountering an interpolation during an unquoted argument... */
      | (Some (Interpolation _), _, _) when (str !== "") => Str str

      /* ...but when the string is empty (next loop) we emit the interpolation */
      | (Some (Interpolation value), _, _) => {
        LazyStream.junk s; /* throw away the interpolation */
        Interpolation value
      }

      /* an unquoted argument must be explicitly ended, thus an EOF is unacceptable */
      | (None, _, _) => {
        raise (LazyStream.Error "Unexpected EOF before end of unquoted argument")
      }
    }
  };

  /* tokenise a string (excluding quotes which are separate tokens) */
  let rec stringLoop (kind: quoteKind) (str: string): tokenValue => {
    switch (LazyStream.peek s) {
      /* escaped content is allowed anywhere inside a string */
      | Some (Char '\\') => {
        LazyStream.junk s; /* throw away leading backslash */
        stringLoop kind (str ^ (captureEscapedContent ()))
      }

      /* exit StringLoop when ending quote is found */
      | Some (Char '\'') when (kind === Single) => {
        state.mode = StringEndLoop;
        Str str
      }
      | Some (Char '\"') when (kind === Double) => {
        state.mode = StringEndLoop;
        Str str
      }

      /* newlines inside strings are not permitted, except when they're escaped */
      | Some (Char '\n') => {
        raise (LazyStream.Error "Expected newline inside string to be escaped")
      }

      /* every char is accepted into the string */
      | Some (Char c) => {
        LazyStream.junk s; /* throw away char */
        stringLoop kind (str ^ (string_of_char c))
      }

      /* emit string and wait for next loop when encountering an interpolation during a string... */
      | Some (Interpolation _) when (str !== "") => Str str

      /* ...but when the string is empty (next loop) we emit the interpolation */
      | Some (Interpolation value) => {
        LazyStream.junk s; /* throw away the interpolation */
        Interpolation value
      }

      /* a string must be explicitly ended, thus an EOF is unacceptable */
      | None => {
        raise (LazyStream.Error "Unexpected EOF before end of string")
      }
    }
  };

  /* capture the quote that comes after a string's end (quote has already been detected) */
  let stringEndLoop (): tokenValue => {
    state.mode = MainLoop;

    switch (LazyStream.next s) {
      | Some (Char '\"') => Quote Double
      | Some (Char '\'') => Quote Single

      | _ => {
        raise (LazyStream.Error "Expected quote at the end of the last string")
      }
    }
  };

  let rec mainLoop (): tokenValue => {
    switch (LazyStream.next s) {
      /* single char tokens */
      | Some (Char '{') => Brace Opening
      | Some (Char '}') => Brace Closing
      | Some (Char '[') => Bracket Opening
      | Some (Char ']') => Bracket Closing
      | Some (Char '!') => Exclamation
      | Some (Char '=') => Equal
      | Some (Char ':') => Colon
      | Some (Char ';') => Semicolon
      | Some (Char '+') => Plus
      | Some (Char '&') => Ampersand
      | Some (Char '>') => Arrow
      | Some (Char '*') => Asterisk
      | Some (Char '~') => Tilde
      | Some (Char ',') => Comma
      | Some (Char '|') => Pipe
      | Some (Char '$') => Dollar

      /* single char token; closing parenthesis, see below for opening */
      | Some (Char ')') => Paren Closing

      /* detect whether parenthesis is part of url() or calc() */
      | Some (Char '(') => {
        skipWhitespaces (); /* skip all leading whitespaces */

        ignore (switch state.lastTokenValue {
          | Word "url" => state.mode = UnquotedArgumentLoop StrictString;
          | Word "calc" => state.mode = UnquotedArgumentLoop LooseNested;
          | _ => ()
        });

        Paren Opening
      }

      /* skip over carriage returns, tabs, and whitespaces */
      | Some (Char '\r')
      | Some (Char '\t')
      | Some (Char ' ') => mainLoop ()

      /* count up current line number and search next tokenValue */
      | Some (Char '\n') => {
        state.line = state.line + 1;
        mainLoop ()
      }

      /* detect double quotes and queue StringLoop */
      | Some (Char '\"') => {
        state.mode = StringLoop Double;
        Quote Double
      }

      /* detect single quotes and queue StringLoop */
      | Some (Char '\'') => {
        state.mode = StringLoop Single;
        Quote Single
      }

      /* detect backslash i.e. escape as start of a word and parse it */
      | Some (Char '\\') => Word (captureEscapedContent ())

      /* detect at-words and parse it like a normal word afterwards */
      | Some (Char '@') => {
        /* at-words will never be composed with interpolations, so no switch to WordLoop */
        let wordContent = captureWordContent "@";
        AtWord wordContent
      }

      /* detect start of a word and parse it */
      | Some (Char c) when (isWordStartChar c) => {
        /* switch to combinator to emit a WordCombinator when no space occurs between interpolations/words */
        state.mode = CombinatorLoop;

        /* capture rest of word */
        let wordContent = captureWordContent (string_of_char c);
        Word wordContent
      }

      /* pass-through interpolation */
      | Some (Interpolation x) => {
        /* switch to combinator to emit a WordCombinator when no space occurs between interpolations/words */
        state.mode = CombinatorLoop;
        Interpolation x
      }

      /* detect and skip comments, then search next tokenValue */
      | Some (Char '/') when (isNextCharEqual '*') => {
        LazyStream.junk s; /* throw away the leading asterisk */
        skipCommentContent ();
        mainLoop ()
      }

      /* all unrecognised characters will be raised outside of designated matching loops */
      | Some (Char c) => {
        let msg = "Unexpected token encountered: " ^ (string_of_char c);
        raise (LazyStream.Error msg)
      }

      | None => EOF
    }
  };

  /* emits a combination token when the last and next token are an interpolation/word */
  let combinatorLoop (): tokenValue => {
    state.mode = MainLoop;

    switch (LazyStream.peek s) {
      /* emit WordCombinator when next token will be an interpolation or word again */
      | Some (Interpolation _) => WordCombinator
      | Some (Char c) when (isWordStartChar c) => WordCombinator

      /* pass over to MainLoop immediately if no word or interpolation followed */
      | _ => mainLoop ()
    }
  };

  /* next function needs to be defined as uncurried and arity-0 at its definition */
  let next: (unit => option token) [@bs] = (fun () => {
    let token = switch state.mode {
      | MainLoop => mainLoop ()
      | CombinatorLoop => combinatorLoop ()
      | StringLoop kind => stringLoop kind ""
      | StringEndLoop => stringEndLoop ()
      | UnquotedArgumentLoop kind => unquotedArgumentLoop kind 0 ""
    };

    switch token {
      | EOF => None /* special token to signalise the end of the stream */
      | value => {
        /* store token as "last token" for unquote argument detection */
        state.lastTokenValue = value;
        Some (Token value state.line)
      }
    }
  }) [@bs];

  LazyStream.from next
};
