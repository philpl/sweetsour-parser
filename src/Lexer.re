open Common;

/* For tokens that are opening or closing the difference can be abstracted w/o their value */
type pairwiseKind =
  | Opening
  | Closing;

/* A token's value represented by its type constructor and the value type */
type tokenValue =
  | Interpolation interpolation
  | Paren pairwiseKind
  | Brace pairwiseKind
  | Bracket pairwiseKind
  | Word string
  | AtWord string
  | Str string
  | Equal
  | Colon
  | Semicolon
  | Plus
  | Ampersand
  | Arrow
  | Asterisk
  | Tilde
  | Comma;

/* A token represented by its value and a line number */
type token = Token tokenValue int;

let lexer = fun (s: Input.inputStream) => {
  /* ref value to keep track of the current line number; must be updated for every newline */
  let line = ref 1;

  /* a buffer holding tokenValues to compute some tokens ahead of time */
  let tokenValueBuffer: ref (list tokenValue) = ref [];

  /* skip over all chars until end of comment is reached */
  let rec skipCommentContent () => {
    switch (Stream.next s) {
      | Char '*' when (Stream.peek s == Some (Char '/')) => {
        Stream.junk s; /* throw away the trailing slash */
      }

      /* count up current line number inside comments */
      | Char '\n' => {
        line := !line + 1;
        skipCommentContent ()
      }

      | _ => skipCommentContent ()

      | exception Stream.Failure => {
        raise (Stream.Error "Unexpected EOF, expected end of comment")
      }
    };
  };

  /* captures any hex code of 1-n digits (spec-compliancy is 1-6), assuming that the code's start is in `str` */
  let rec captureHexDigits (str: string): string => {
    switch (Stream.peek s) {
      | Some (Char ('a'..'f' as c))
      | Some (Char ('A'..'F' as c))
      | Some (Char ('0'..'9' as c)) => {
        Stream.junk s;
        captureHexDigits (str ^ (String.make 1 c))
      }

      /* hex code escapes are optionally followed by an extra whitespace */
      | Some (Char ' ') => {
        Stream.junk s;
        str ^ " "
      }

      | Some _
      | None => str
    }
  };

  /* captures the content of an escape, assuming a backslash preceded it */
  let captureEscapedContent (): string => {
    let escaped = switch (Stream.next s) {
      /* detect start of a hex code as those have a different escape syntax */
      | Char ('A'..'F' as c)
      | Char ('a'..'f' as c)
      | Char ('0'..'9' as c) => {
        captureHexDigits (String.make 1 c)
      }

      /* count up current line number even for escaped newlines */
      | Char '\n' => {
        line := !line + 1;
        "\n"
      }

      | Char c => String.make 1 c

      /* it's too risky to allow value-interpolations as part of escapes */
      | Interpolation _ => {
        raise (Stream.Error "Unexpected interpolation after backslash, expected escaped content")
      }

      | exception Stream.Failure => {
        raise (Stream.Error "Unexpected EOF after backslash, expected escaped content")
      }
    };

    "\\" ^ escaped
  };

  /* captures the content of a string, `quote` being the terminating character */
  let rec captureStringContent (quote: char) (str: string): string => {
    switch (Stream.next s) {
      /* escaped content is allowed anywhere inside a string */
      | Char '\\' => {
        captureStringContent quote (str ^ (captureEscapedContent ()))
      }

      | Char c => {
        /* check if end of string is reached or continue */
        if (c === quote) {
          str
        } else {
          captureStringContent quote (str ^ (String.make 1 c))
        }
      }

      /* we only have a value-interpolation, which is not the same as an interpolation inside a string */
      /* the only sane thing to do is to disallow interpolations inside strings so that the user replaces the entire string */
      | Interpolation _ => {
        raise (Stream.Error "Unexpected interpolation inside a string")
      }

      | exception Stream.Failure => {
        raise (Stream.Error "Unexpected EOF before end of string")
      }
    }
  };

  let captureURLToken (previousWordContent: string) => {
    if (previousWordContent === "url" && Stream.peek s == Some (Char '(')) {
      Stream.junk s; /* Skip over opening paren */
      tokenValueBuffer := [Paren Opening, ...!tokenValueBuffer];

      /* capture contents of url() argument until closing paren */
      let urlContent = captureStringContent ')' "";
      tokenValueBuffer := [Str urlContent, ...!tokenValueBuffer];

      /* add closing paren that captureStringContent skipped over */
      tokenValueBuffer := [Paren Closing, ...!tokenValueBuffer];
    }
  };

  /* captures the content of a word, assuming that the word's start is captured in `str` */
  let rec captureWordContent (str: string): string => {
    switch (Stream.peek s) {
      /* escaped content is allowed anywhere inside a word */
      | Some (Char '\\') => {
        Stream.junk s;
        captureWordContent (str ^ (captureEscapedContent ()))
      }

      /* the following ranges of chars are part of the word's tail */
      | Some (Char ('a'..'z' as c))
      | Some (Char ('A'..'Z' as c))
      | Some (Char ('0'..'9' as c))
      | Some (Char ('#' as c))
      | Some (Char ('.' as c))
      | Some (Char ('%' as c))
      | Some (Char ('-' as c))
      | Some (Char ('_' as c)) => {
        Stream.junk s;
        captureWordContent (str ^ (String.make 1 c))
      }

      | Some _
      | None => {
        str
      }
    }
  };

  let rec nextTokenValue (): tokenValue => {
    switch (Stream.next s) {
      /* single character tokens */
      | Char '(' => Paren Opening
      | Char ')' => Paren Closing
      | Char '{' => Brace Opening
      | Char '}' => Brace Closing
      | Char '[' => Bracket Opening
      | Char ']' => Bracket Closing
      | Char '=' => Equal
      | Char ':' => Colon
      | Char ';' => Semicolon
      | Char '+' => Plus
      | Char '&' => Ampersand
      | Char '>' => Arrow
      | Char '*' => Asterisk
      | Char '~' => Tilde
      | Char ',' => Comma

      /* skip over carriage return and whitespace */
      | Char '\r'
      | Char ' ' => nextTokenValue ()

      /* count up current line number and search next tokenValue */
      | Char '\n' => {
        line := !line + 1;
        nextTokenValue ()
      }

      | Char ('\"' as c)
      | Char ('\'' as c) => {
        let stringContent = captureStringContent c "";
        Str stringContent
      }

      /* detect backslash i.e. escape as start of a word and parse it */
      | Char '\\' => Word (captureEscapedContent ())

      /* detect start of a word and parse it */
      | Char ('a'..'z' as c)
      | Char ('A'..'Z' as c)
      | Char ('0'..'9' as c)
      | Char ('#' as c)
      | Char ('!' as c)
      | Char ('.' as c) => {
        let wordContent = captureWordContent (String.make 1 c);
        captureURLToken wordContent; /* handle url() argument */
        Word wordContent
      }

      /* detect at-words and parse it like a normal word afterwards */
      | Char '@' => {
        let wordContent = captureWordContent "@";
        captureURLToken wordContent; /* handle url() argument */
        AtWord wordContent
      }

      /* pass-through interpolation */
      | Interpolation x => Interpolation x

      /* detect and skip comments, then search next tokenValue */
      | Char '/' when (Stream.peek s == Some (Char '*')) => {
        Stream.junk s; /* throw away the leading asterisk */
        skipCommentContent ();
        nextTokenValue ()
      }

      | Char c => {
        let msg = "Unexpected token encountered: " ^ (String.make 1 c);
        raise (Stream.Error msg)
      }
    }
  };

  let next _: option token => {
    switch !tokenValueBuffer {
      /* empty tokenValue buffer before scanning new tokens */
      | [bufferedItem, ...rest] => {
        tokenValueBuffer := rest;
        Some (Token bufferedItem !line)
      }

      /* get next token and return it, except if stream is empty */
      | [] => switch (nextTokenValue ()) {
        | value => Some (Token value !line)
        | exception Stream.Failure => None
      }
    }
  };

  Stream.from next
};