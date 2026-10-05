# Potreban alat za izradu jeste avr

Za MacOS je potrebno uraditi u terminalu

brew trust osx-cross/avr
brew install avr-gcc

'''
cat <<'EOF' > .clangd
CompileFlags:
  CompilationDatabase: /Users/ognjengutesa/Developments/espavr/build
  Remove: [-m*, -f*]
  Add:
    - -isystem/opt/homebrew/opt/avr-gcc@9/avr/include
    - -D__AVR_ATtiny85__
    - -DF_CPU=1000000UL
EOF
'''