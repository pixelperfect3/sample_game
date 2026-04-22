# Sample Game — Claude Code Instructions                                                                                                                                                                                                               
                                                                                                                                                                                                                                                     
This project uses the Sama engine. When writing engine code:                                                                                                                                                                                       
                                                                
## Engine API Reference                                                                                                                                                                                                                            
                                                                                                                                                                                                                                                   
- **Primary reference:** `build/_deps/sama-src/docs/AI_NATIVE.md`
  - Section 2: API cheat sheet (copy-pasteable code for entities, rendering, physics, audio, animation)                                                                                                                                            
  - Section 4: Minimal game template                                                                                                                                                                                                               
  - Section 5: Component reference table    
  - Section 6: Common pitfalls — read before submitting code                                                                                                                                                                                       
                                                                
- **Architecture docs:** `build/_deps/sama-src/docs/`                                                                                                                                                                                              
  - `GAME_LAYER_ARCHITECTURE.md` — IGame, GameRunner, SceneManager
  - `ANIMATION_ARCHITECTURE.md` — skeletal animation, IK                                                                                                                                                                                           
  - `PHYSICS_ARCHITECTURE.md` — Jolt integration                                                                                                                                                                                                   
  - `NOTES.md` — engine decisions                                                                                                                                                                                                                  
                                                                                                                                                                                                                                                   
## Engine Conventions (from Sama)                                                                                                                                                                                                                  
                                                                                                                                                                                                                                                   
- Always add `WorldTransformComponent` alongside `TransformComponent`                                                                                                                                                                              
- Always add `VisibleTag` for entities that should render                                                                                                                                                                                          
- Always call `assets.processUploads()` each frame after async loading                                                                                                                                                                             
- Set `tc.flags |= 1` (dirty) after modifying transforms              
                                                                                                                                                                                                                                                   
## Sample Game's Conventions                                        
                                                                                                                                                                                                                                                   
- Game logic goes in `SampleGame.cpp` (IGame callbacks)                                                                                                                                                                                                
- Custom components go in `components/` directory                                                                                                                                                                                                  
- Custom systems go in `systems/` directory                                                                                                                                                                                                        
- C++20, Allman braces, 4-space indent, 100 char line limit (same as Sama)                                                                                                                                                                         
                                                                                                                                                                                                                                                   
## Git Workflow

- Always push to GitHub whenever changes are made

## Commands                                 
                                                                                                                                                                                                                                                   
- Build: `cmake --build build --target sample_game -j$(sysctl -n hw.ncpu)`
- Run: `./build/sample_game`
