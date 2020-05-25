import os
from graphviz import Digraph
def screenSource(raw):
    return raw.split('.')[0]
def screenTarget(raw, restriction):
    selfDef = raw.split('\"')
    if(len(selfDef) > 1):
        include = selfDef[1].split('/')
        if(include[0] == restriction):
            return include[1].split('.')[0]
    return ""
def screenAlotTarget(raw, restriction):
    selfDef = raw.split('\"') # scrren the included part
    if(len(selfDef) > 1):
        include = selfDef[1].split('/')
        for r in restriction: # screen sources included from other directory
            if(include[0] == r):
                if(include[0] == os.getcwd().split('/')[-1]):
                    return include[1].split('.')[0]
                else:
                    return r
    return ""
g = Digraph('G', filename='test.gv')
g.edge_attr.update(arrowhead='vee')
strongRelation = False
for filename in os.listdir(os.getcwd()):
    # print(filename)
    if(filename.endswith(('.h'))):
        strongRelation = True
    elif(filename.endswith(('.cpp'))):
        strongRelation = False
    else:
        continue
    with open(os.path.join(os.getcwd(), filename), 'r') as f: # open in readonly mode
        lines = f.readlines()
        for line in lines:
            if line.split(' ')[0] == '#include':
                # target = screenTarget(line, os.getcwd().split('/')[-1])
                target = screenAlotTarget(line, ["herder"])
                source = screenSource(filename)
                if(len(target) > 0):
                    if(source != target):
                        if(strongRelation):
                            g.edge(source ,target)
                        else:
                            g.edge(source, target, style='dashed')
        # do your stuff
g.view()
