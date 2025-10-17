# Tank Battle Game

A two-player tank game where each player runs in their own process.


## Demo  

![Gameplay Demo](tank2.gif)

## Requirements
- GCC compiler
- ncurses library
- Linux/Unix environment

## Installation
For Debian based distros
```bash
sudo apt-get install libncurses5-dev libncurses-dev
```


## Compilation

```bash
make
```

or 
```bash
gcc -Wall -g -o game game.c -lncurses -lpthread
```


## Running the Game
Each player needs to run the game in a separate terminal:

**Player A:**
```bash
make run1
```
or 

```bash
./game map.txt A w s a d f
```
You can put any keybinds that you want

**Player B:**
```bash
make run2
```
or

```bash
./game map.txt B i k j l space
```

## Controls

**Player A:**
- `w,s,a,d` - Movement
- `f` - Fire

**Player B:**
- `i,k,j,l` - Movement
- `space` - Fire

**Both:**
- `q` - Quit

## Game Rules
- Each player has 5 HP
- Hit opponent with projectiles to reduce their HP
- First to reach 0 HP loses
- If one player quits, the other player is automatically notified and the game ends

## Features
- Each player runs in a separate process
- Shared memory for game state
- Protected player positions
- Safe cleanup when either player exits
